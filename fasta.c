// ------------------------------------------------------------------
//   fasta.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "fast_private.h"
#include "seg.h"
#include "move_to_front.h"
#include "file.h"
#include "piz.h"
#include "dict_id.h"

void fasta_seg_initialize (VBlockFAST *vb)
{
    // thread safety: this will be initialized by vb_i=1, while it holds a mutex in zip_compress_one_vb
    static bool structured_initialized = false;
    if (!structured_initialized) {
        seg_initialize_compound_structured ((VBlockP)vb, "D?ESC", &structured_DESC); 
        structured_initialized = true;
    }

    MtfContext *ctx = mtf_get_ctx (vb, (DictIdType)dict_id_FASTA_SEQ);
    ctx->flags  = CTX_FL_LOCAL_LZMA;
    ctx->ltype  = CTX_LT_SEQUENCE;
}

// Fasta format(s): https://en.wikipedia.org/wiki/FASTA_format
// concept: we segment each line separately, and for each line, we store an element in TEMPLATE about it. The
// Metadata elements are:
// > - description line - this (1) any line starting with > or (2) the first line starting with ; at the start 
//     of a file or after a sequence
//     the descrition line data is further segmented and stored in the DESC dictionary and D0SEC subfields
// ; - a comment line - any other line that starts with a ; or an empty line
//     the comment data (which can be empty for an empty line) is stored in a data buffer (not dictionary)
//     note: if a comment line is the first line in a VB - it will be segmented as a description. No harm done.
// 123 - a sequence line - any line that's not a description of sequence line - store its length
// these ^ are preceded by a 'Y' if the line has a Windows-style \r\n line ending or 'X' if not
const char *fasta_seg_txt_line (VBlockFAST *vb, const char *line_start, bool *has_13) // index in vb->txt_data where this line starts
{
    // get entire line
    unsigned line_len;
    int32_t remaining_vb_txt_len = AFTERENT (char, vb->txt_data) - line_start;
    const char *next_field = seg_get_next_line (vb, line_start, &remaining_vb_txt_len, &line_len, has_13, "FASTA line");
    char special_snip[100];
    unsigned special_snip_len;

    // case: description line - we segment it to its components
    // note: we store the DESC structured in its own ctx rather than just directly in LINEMETA, to make it easier to grep
    if (*line_start == '>' || (*line_start == ';' && vb->last_line == FASTA_LINE_SEQ)) {
        // we segment using / | : and " " as separators. 
        seg_compound_field ((VBlockP)vb, mtf_get_ctx (vb, (DictIdType)dict_id_FASTA_DESC), 
                            line_start, line_len, &vb->desc_mapper, structured_DESC, true, 0);
        
        seg_prepare_snip_other (SNIP_REDIRECTION, (DictIdType)dict_id_FASTA_DESC, 0, &special_snip[2], &special_snip_len);

        special_snip[0] = SNIP_SPECIAL;
        special_snip[1] = FASTA_SPECIAL_DESC;

        seg_by_did_i (vb, special_snip, special_snip_len+2, FASTA_LINEMETA, 0);

        SEG_EOL (FASTA_EOL, true);
        vb->last_line = FASTA_LINE_DESC;
    }

    // case: comment line - stored in the comment buffer
    else if (*line_start == ';' || !line_len) {
        seg_add_to_local_text ((VBlockP)vb, mtf_get_ctx (vb, (DictIdType)dict_id_FASTA_COMMENT), 
                               line_start, line_len, line_len); 

        seg_prepare_snip_other (SNIP_OTHER_LOOKUP, (DictIdType)dict_id_FASTA_COMMENT, 0, &special_snip[2], &special_snip_len);

        special_snip[0] = SNIP_SPECIAL;
        special_snip[1] = FASTA_SPECIAL_COMMENT;

        seg_by_did_i (vb, special_snip, special_snip_len+2, FASTA_LINEMETA, 0);
        
        SEG_EOL (FASTA_EOL, true);
        vb->last_line = FASTA_LINE_COMMENT;
    }

    // case: sequence line
    else {
        DATA_LINE (vb->line_i)->seq_data_start = line_start - vb->txt_data.data;
        DATA_LINE (vb->line_i)->seq_len        = line_len;

        MtfContext *seq_ctx = mtf_get_ctx (vb, (DictIdType)dict_id_FASTA_SEQ);
        seq_ctx->local.len += line_len;
        seq_ctx->txt_len   += line_len;

        seg_prepare_snip_other (SNIP_OTHER_LOOKUP, (DictIdType)dict_id_FASTA_SEQ, line_len, &special_snip[3], &special_snip_len);

        special_snip[0] = SNIP_SPECIAL;
        special_snip[1] = FASTA_SPECIAL_SEQ;
        special_snip[2] = '0' + (vb->last_line != FASTA_LINE_SEQ); // first seq line in this contig
        seg_by_did_i (vb, special_snip, 3 + special_snip_len, FASTA_LINEMETA, 0);  // the payload of the special snip, is the OTHER_LOOKUP snip...

        SEG_EOL (FASTA_EOL, true); 
        vb->last_line = FASTA_LINE_SEQ;
    }

    return next_field;
}

// returns true if section is to be skipped reading / uncompressing
bool fasta_piz_is_skip_section (VBlockP vb, SectionType st, DictIdType dict_id)
{
    if (!vb) return false; // we don't skip reading any SEC_DICT sections

    // note that piz_read_global_area rewrites --header-only as flag_header_one
    if (flag_header_one && (dict_id.num == dict_id_FASTA_SEQ || dict_id.num == dict_id_FASTA_COMMENT))
        return true;

    // when grepping by I/O thread - skipping all sections but DESC
    if (vb && flag_grep && (vb->grep_stages == GS_TEST) && 
        dict_id.num != dict_id_FASTA_DESC && !dict_id_is_fast_desc_sf (dict_id))
        return true;

    // if grepping, compute thread doesn't need to decompressed DESC again
    if (vb && flag_grep && (vb->grep_stages == GS_UNCOMPRESS) && 
        (dict_id.num == dict_id_FASTA_DESC || dict_id_is_fast_desc_sf (dict_id)))
        return true;

    return false;
}

// this is used for end-of-lines of a sequence line, that are not the last line of the sequence. we skip reconstructing
// the newline if the user selected --sequencial
void fasta_piz_special_SEQ (VBlock *vb_, MtfContext *ctx, const char *snip, unsigned snip_len)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;

    bool is_first_seq_line_in_this_contig = snip[0] - '0';

    // --sequential - if this is NOT the first seq line in the contig, we delete the previous end-of-line
    // TO DO: this doesn't yet work across vblocks boundaries
    if (flag_fasta_sequential && !is_first_seq_line_in_this_contig) {
        if (vb->txt_data.len && *LASTENT (char, vb->txt_data) == '\n') vb->txt_data.len--;
        if (vb->txt_data.len && *LASTENT (char, vb->txt_data) == '\r') vb->txt_data.len--;
    }

    // skip showing line if this contig is grepped - but consume it anyway
    if (vb->contig_grepped_out) vb->dont_show_curr_line = true;

    // in case of not showing the SEQ in the entire file - we can skip consuming it
    if (flag_header_one) // note that piz_read_global_area rewrites --header-only as flag_header_one
        vb->dont_show_curr_line = true;     
    else 
        piz_reconstruct_one_snip (vb_, ctx, snip+1, snip_len-1);    
}

void fasta_piz_special_COMMENT (VBlock *vb_, MtfContext *ctx, const char *snip, unsigned snip_len)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;

    // skip showing line if this contig is grepped - but consume it anyway
    if (vb->contig_grepped_out) vb->dont_show_curr_line = true;

    // in case of not showing the COMMENT in the entire file - we can skip consuming it
    if (flag_header_one)  // note that piz_read_global_area rewrites --header-only as flag_header_one
        vb->dont_show_curr_line = true;     
    else 
        piz_reconstruct_one_snip (vb_, ctx, snip, snip_len);    
}

// this is called by fast_piz_test_grep - it is called sequentially for all VBs by the I/O thread
// returns true if the last contig of the previous VB was grepped-in
bool fasta_initialize_contig_grepped_out (VBlockFAST *vb, bool does_vb_have_any_desc, bool last_desc_in_this_vb_matches_grep)
{
    // we pass the info from one VB to the next using this static variable
    static bool prev_vb_last_contig_grepped_out = false; 
    bool ret = !prev_vb_last_contig_grepped_out;

    // we're continuing the contig in the previous VB - until DESC is encountered
    vb->contig_grepped_out = prev_vb_last_contig_grepped_out; 
    
    // update for use of next VB, IF this VB contains any DESC line, otherwise just carry forward the current value
    if (does_vb_have_any_desc) 
        prev_vb_last_contig_grepped_out = !last_desc_in_this_vb_matches_grep; 

    return ret;
}

void fasta_piz_special_DESC (VBlock *vb_, MtfContext *ctx, const char *snip, unsigned snip_len)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;

    const char *desc_start = AFTERENT (const char, vb->txt_data);
    piz_reconstruct_one_snip (vb_, ctx, snip, snip_len);    

    // if --grep: here we decide whether to show this contig or not
    if (flag_grep) {
        *AFTERENT (char, vb->txt_data) = 0; // for strstr
        vb->contig_grepped_out = !strstr (desc_start, flag_grep);
    }

    // note: this logic allows the to grep contigs even if --no-header
    if (vb->contig_grepped_out || flag_no_header) vb->dont_show_curr_line = true;     
}

void fasta_piz_reconstruct_vb (VBlockFAST *vb)
{
    if (!flag_grep) // if --grep this is already done in fast_piz_test_grep
        piz_map_compound_field ((VBlockP)vb, dict_id_is_fast_desc_sf, &vb->desc_mapper); 

    for (vb->line_i=vb->first_line; vb->line_i < vb->first_line + vb->lines.len; vb->line_i++) {
        vb->dont_show_curr_line = false; // might be made true by fasta_piz_special_*
        uint32_t txt_data_start_line = vb->txt_data.len;

        piz_reconstruct_from_ctx (vb, FASTA_LINEMETA, 0);
        piz_reconstruct_from_ctx (vb, FASTA_EOL, 0);

        if (vb->dont_show_curr_line)
            vb->txt_data.len = txt_data_start_line; // rollback
    }
}
