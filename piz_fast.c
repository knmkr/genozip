// ------------------------------------------------------------------
//   piz_fast.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "profiler.h"
#include "zfile.h"
#include "txtfile.h"
#include "header.h"
#include "vblock.h"
#include "base250.h"
#include "move_to_front.h"
#include "file.h"
#include "endianness.h"
#include "piz.h"
#include "sections.h"
#include "dict_id.h"

static const char *eol[2] = { "\n", "\r\n"};
static const unsigned eol_len[2] = { 1, 2 };

// called by I/O thread in zfile_fast_read_one_vb, in case of --grep, to decompress and reconstruct the desc line, to 
// see if this vb is included. 
bool piz_fastq_test_grep (VBlockFAST *vb)
{
    ARRAY (const unsigned, section_index, vb->z_section_headers);

    SectionHeaderVbHeader *header = (SectionHeaderVbHeader *)(vb->z_data.data + section_index[0]);
    vb->first_line       = BGEN32 (header->first_line);
    vb->lines.len        = BGEN32 (header->num_lines);
    vb->vb_data_size     = BGEN32 (header->vb_data_size);
    vb->longest_line_len = BGEN32 (header->longest_line_len);

    // in case of --split, the vblock_i in the 2nd+ component will be different than that assigned by the dispatcher
    // because the dispatcher is re-initialized for every sam component
    if (flag_split) vb->vblock_i = BGEN32 (header->h.vblock_i);
    
    unsigned section_i=1;

    // we only need room for one line for now 
    buf_alloc (vb, &vb->txt_data, vb->longest_line_len, 1.1, "txt_data", vb->vblock_i);

    // uncompress the fields     
    piz_uncompress_fields ((VBlockP)vb, section_index, &section_i);
    
    // uncompress DESC subfields
    piz_uncompress_compound_field ((VBlockP)vb, SEC_FAST_DESC_B250, SEC_FAST_DESC_SF_B250, &vb->desc_mapper, &section_i);

    // reconstruct each description line and check for string matching with flag_grep
    bool found = false;
    for (uint32_t vb_line_i=0; vb_line_i < vb->mtf_ctx[FAST_DESC].b250.len; vb_line_i++) {
        const char *snip;
        unsigned snip_len;
        uint32_t txt_line_i = 4 * (vb->first_line + vb_line_i);

        LOAD_SNIP (FAST_DESC);
        piz_reconstruct_compound_field ((VBlockP)vb, &vb->desc_mapper, 0, 0, snip, snip_len, txt_line_i);

        *AFTERENT (char, vb->txt_data) = 0; // terminate the desc string

        bool match = !!strstr (vb->txt_data.data, flag_grep);

        vb->txt_data.len = 0; // reset

        if (match) { 
            found = true; // we've found a match to the grepped string
            break;
        }
    }

    // reset iterators - piz_fastq_reconstruct_vb will use them again 
    mtf_init_iterator (&vb->mtf_ctx[FAST_DESC]);
    for (unsigned sf_i=0; sf_i < vb->desc_mapper.num_subfields; sf_i++)
        mtf_init_iterator (&vb->mtf_ctx[vb->desc_mapper.did_i[sf_i]]);

    return found; // no match found
}

static void piz_fastq_reconstruct_vb (VBlockFAST *vb)
{
    START_TIMER;

    buf_alloc (vb, &vb->txt_data, vb->vb_data_size, 1.1, "txt_data", vb->vblock_i);
    
    for (uint32_t vb_line_i=0; vb_line_i < vb->lines.len; vb_line_i++) {

        uint32_t snip_len;
        const char *snip;

        uint32_t txt_line_i = 4 * (vb->first_line + vb_line_i); // each vb line is a fastq record which is 4 txt lines
        
        uint32_t txt_data_start_line = vb->txt_data.len;

        // metadata looks like this - "X151YXX" - the 4 X,Y characters specify whether each row has a \r (Y=has)
        // and the number is the seq_len=qual_len
        LOAD_SNIP (FAST_LINEMETA);
        const char *md = snip;

        // description line
        if (!flag_strip) {
            LOAD_SNIP (FAST_DESC);
            piz_reconstruct_compound_field ((VBlockP)vb, &vb->desc_mapper, eol[md[0]-'X'], eol_len[md[0]-'X'], 
                                            snip, snip_len, txt_line_i);
            *AFTERENT (char, vb->txt_data) = 0; // null-terminate for sec, in case of grep
        }

        bool grepped_out = false;
        // case: we're grepping, and this line doesn't match
        if (flag_grep && !strstr (&vb->txt_data.data[txt_data_start_line], flag_grep)) { 
            vb->txt_data.len = txt_data_start_line; // rollback
            grepped_out = true;
        }

        if (flag_header_one) continue; // this is invoked by --header-only (re-written to flag_header_one in piz_read_global_area)

        // sequence line
        uint32_t seq_len = atoi (&md[4]); // numeric string terminated by dictionary's \t separator
        piz_reconstruct_seq_qual ((VBlockP)vb, seq_len, &vb->seq_data, &vb->next_seq, SEC_SEQ_DATA, txt_line_i, grepped_out);
        if (!grepped_out) buf_add (&vb->txt_data, eol[md[1]-'X'], eol_len[md[1]-'X']); // end of line

        if (!flag_strip) {
            // + line
            if (!grepped_out) buf_add (&vb->txt_data, md[2]-'X' ? "+\r\n" : "+\n", eol_len[md[2]-'X'] + 1);

            // quality line
            piz_reconstruct_seq_qual ((VBlockP)vb, seq_len, &vb->qual_data, &vb->next_qual, SEC_QUAL_DATA, txt_line_i, grepped_out);
            
            if (!grepped_out) buf_add (&vb->txt_data, eol[md[3]-'X'], eol_len[md[3]-'X']); // end of line
        }

    }

    COPY_TIMER(vb->profile.piz_reconstruct_vb);
}

static void piz_fasta_reconstruct_vb (VBlockFAST *vb)
{
    // note: we cannot easily do grep for FASTA, because records might span multiple VBs - the second+ VB doesn't have
    // access to the description to compare

    START_TIMER;

    buf_alloc (vb, &vb->txt_data, vb->vb_data_size, 1.1, "txt_data", vb->vblock_i);
    
    for (uint32_t vb_line_i=0; vb_line_i < vb->lines.len; vb_line_i++) {

        uint32_t snip_len;
        const char *snip;

        uint32_t txt_line_i = vb->first_line + vb_line_i;

        // metadata looks like this - "X>" (desc line), "X;" (comment line) "X123" (sequence line)
        // X, Y characters specify whether each row has a \r (Y=has)
        LOAD_SNIP (FAST_LINEMETA);
        const char *md = snip;
        bool has_13 = md[0] - 'X';

        switch (md[1]) {
            case '>': // description line 
                if (!flag_strip) {
                    LOAD_SNIP (FAST_DESC);
                    piz_reconstruct_compound_field ((VBlockP)vb, &vb->desc_mapper, eol[has_13], eol_len[has_13], 
                                                    snip, snip_len, txt_line_i);
                    vb->last_line = FASTA_LINE_DESC;
                }
                break;

            case ';': // comment line
                if (!flag_strip && !flag_header_one) 
                    RECONSTRUCT_FROM_BUF (vb->comment_data, vb->next_comment, "COMMENT", '\n', eol[has_13], eol_len[has_13]);

                //if (flag_header_one && !snip_len)
                //    vb->txt_data.len -= eol_len[has_13]; // don't show empty lines in --header-only mode

                vb->last_line = FASTA_LINE_COMMENT;
                break;

            default:  // sequence line
                if (!flag_header_one) { // this is invoked by --header-only (re-written to flag_header_one in piz_read_global_area)

                    if (flag_fasta_sequential && vb->last_line == FASTA_LINE_SEQ && vb->txt_data.len >= 2)
                        vb->txt_data.len -= 1 + (vb->txt_data.data[vb->txt_data.len-2]=='\r');

                    uint32_t seq_len = atoi (&md[1]); // numeric string terminated by dictionary's \t separator
                    piz_reconstruct_seq_qual ((VBlockP)vb, seq_len, &vb->seq_data, &vb->next_seq, SEC_SEQ_DATA, txt_line_i, false);
                    buf_add (&vb->txt_data, eol[has_13], eol_len[has_13]); // end of line
                    vb->last_line = FASTA_LINE_SEQ;
                }
        }
    }

    COPY_TIMER(vb->profile.piz_reconstruct_vb);
}

static void piz_fast_uncompress_all_sections (VBlockFAST *vb)
{
    ARRAY (const unsigned, section_index, vb->z_section_headers);

    SectionHeaderVbHeader *header = (SectionHeaderVbHeader *)(vb->z_data.data + section_index[0]);
    vb->first_line       = BGEN32 (header->first_line);
    vb->lines.len        = BGEN32 (header->num_lines);
    vb->vb_data_size     = BGEN32 (header->vb_data_size);
    vb->longest_line_len = BGEN32 (header->longest_line_len);

    // in case of --split, the vblock_i in the 2nd+ component will be different than that assigned by the dispatcher
    // because the dispatcher is re-initialized for every sam component
    if (flag_split) vb->vblock_i = BGEN32 (header->h.vblock_i);
    
    unsigned section_i=1;

    // uncompress the fields     
    if (!flag_grep) // if flag_grep - the DESC fields were already uncompressed by the I/O thread
        piz_uncompress_fields ((VBlockP)vb, section_index, &section_i);
    else 
        section_i += NUM_FAST_FIELDS;
    
    // DESC subfields
    if (!flag_grep) // if flag_grep - the DESC subfields were already uncompressed by the I/O thread
        piz_uncompress_compound_field ((VBlockP)vb, SEC_FAST_DESC_B250, SEC_FAST_DESC_SF_B250, &vb->desc_mapper, &section_i);
    else
        section_i += vb->desc_mapper.num_subfields;

    // SEQ    
    SectionHeader *seq_header = (SectionHeader *)(vb->z_data.data + section_index[section_i++]);
    zfile_uncompress_section ((VBlockP)vb, seq_header, &vb->seq_data, "seq_data", SEC_SEQ_DATA);    

    // QUAL (FASTQ only)
    if (vb->data_type == DT_FASTQ) {
        SectionHeader *qual_header = (SectionHeader *)(vb->z_data.data + section_index[section_i++]);
        zfile_uncompress_section ((VBlockP)vb, qual_header, &vb->qual_data, "qual_data", SEC_QUAL_DATA);    
    }

    // COMMENT (FASTA only)
    else {
        SectionHeader *comment_header = (SectionHeader *)(vb->z_data.data + section_index[section_i++]);
        zfile_uncompress_section ((VBlockP)vb, comment_header, &vb->comment_data, "comment_data", SEC_FASTA_COMMENT_DATA);    
    }
}

void piz_fast_uncompress_one_vb (VBlock *vb_)
{
    START_TIMER;

    VBlockFAST *vb = (VBlockFAST *)vb_;

    piz_fast_uncompress_all_sections ((VBlockFASTP)vb);

    if (vb->data_type == DT_FASTQ)
        piz_fastq_reconstruct_vb ((VBlockFASTP)vb);
    else
        piz_fasta_reconstruct_vb ((VBlockFASTP)vb);

    vb->is_processed = true; // tell dispatcher this thread is done and can be joined. this operation needn't be atomic, but it likely is anyway
    
    COPY_TIMER (vb->profile.compute);
}