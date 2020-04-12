// ------------------------------------------------------------------
//   seg_vcf.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "seg.h"
#include "vblock.h"
#include "move_to_front.h"
#include "header.h"
#include "random_access_vcf.h"
#include "optimize_vcf.h"

#define DATA_LINE(vb,i) (&((ZipDataLineVCF *)((vb)->data_lines))[(i)])

static inline uint32_t seg_vcf_one_field (VBlockVCF *vb, const char *str, unsigned len, unsigned vb_line_i, VcfFields f, 
                                          bool *is_new)  // optional out
{
    return seg_one_field ((VBlockP)vb, str, len, vb_line_i, f, SEC_VCF_CHROM_B250 + f*2, is_new);
}                                          
                               
// returns true if this line has the same chrom as this VB, or if it is the first line
static void seg_chrom_field (VBlockVCF *vb, const char *chrom_str, unsigned chrom_str_len, unsigned vb_line_i)
{
    ASSERT0 (chrom_str_len, "Error in seg_chrom_field: chrom_str_len=0");

    uint32_t chrom_node_index = seg_vcf_one_field (vb, chrom_str, chrom_str_len, vb_line_i, VCF_CHROM, NULL);

    random_access_update_chrom (vb, vb_line_i, chrom_node_index);
}

// traverses the FORMAT field, gets ID of subfield, and moves to the next subfield
DictIdType seg_get_format_subfield (const char **str, uint32_t *len, // remaining length of line
                                    unsigned vb_line_i) 
{
    DictIdType subfield = { 0 };

    unsigned i=0; for (; i < *len; i++) {
        if ((*str)[i] != ':' && (*str)[i] != '\t' && (*str)[i] != '\n') { // note: in vcf files, we will see \t at end of FORMAT, but in variant data sections of piz files, we will see a \n
            
            if (i < DICT_ID_LEN) // note, a subfield might be longer that DICT_ID_LEN, but we copy only the first DICT_ID_LEN characters
                subfield.id[i] = (*str)[i];
        } 
        else break;
    }

    *str += i+1;
    *len -= i+1;
    return subfield; 
}

static void seg_format_field (VBlockVCF *vb, ZipDataLineVCF *dl, 
                              const char *field_start, int field_len, 
                              unsigned vb_line_i)
{
    const char *str = field_start;
    int len = field_len;
    SubfieldMapper format_mapper;
    memset (&format_mapper, 0, sizeof (format_mapper));

    if (!global_vcf_num_samples) return; // if we're not expecting any samples, we ignore the FORMAT field

    ASSERT (field_len, "Error: missing FORMAT field in vb_line_i=%u", vb_line_i);

    if (field_len >= 2 && str[0] == 'G' && str[1] == 'T' && (field_len == 2 || str[2] == ':')) // GT field in FORMAT columns - must always appear first per VCF spec (if at appears)
        dl->has_haplotype_data = true; 

    // we have genotype data, iff FORMAT is not "GT" or ""
    if (field_len > 2 || (!dl->has_haplotype_data && field_len > 0)) {
        dl->has_genotype_data = true; // no GT subfield, or subfields in addition to GT

        if (dl->has_haplotype_data) {
            str +=3; // skip over GT
            len -= 3;
        }

        do {
            ASSERT (format_mapper.num_subfields < MAX_SUBFIELDS, 
                    "Error: FORMAT field in vb_line_i=%u has too many subfields, the maximum allowed is %u (excluding GT)", vb_line_i, MAX_SUBFIELDS);

            DictIdType subfield = seg_get_format_subfield (&str, (unsigned *)&len, vb_line_i);

            ASSERT (dict_id_is_vcf_format_sf (subfield), 
                    "Error: string %.*s in the FORMAT field of vb_line_i=%u is not a legal subfield", DICT_ID_LEN, subfield.id, vb_line_i);

            MtfContext *ctx = mtf_get_ctx_by_dict_id (vb->mtf_ctx, &vb->num_dict_ids, &vb->num_format_subfields, subfield, SEC_VCF_FRMT_SF_DICT);
            
            format_mapper.did_i[format_mapper.num_subfields++] = ctx ? ctx->did_i : (uint8_t)NIL;
        } 
        while (str[-1] != '\t' && str[-1] != '\n' && len > 0);
    }

    if (dl->has_genotype_data)
        buf_alloc (vb, &vb->line_gt_data, format_mapper.num_subfields * global_vcf_num_samples * sizeof(uint32_t), 1, "line_gt_data", vb_line_i); 

    bool is_new;
    uint32_t node_index = seg_vcf_one_field (vb, field_start, field_len, vb_line_i, VCF_FORMAT, &is_new);

    dl->format_mtf_i = node_index;

    // if this is a new format - add mapper
    if (is_new) {
        ASSERT (node_index == vb->format_mapper_buf.len, 
                "Error: node_index=%u different than vb->format_mapper_buf.len=%u", node_index, vb->format_mapper_buf.len);

        vb->format_mapper_buf.len++;
        buf_alloc (vb, &vb->format_mapper_buf, vb->format_mapper_buf.len * sizeof (SubfieldMapper), 2, "format_mapper_buf", 0);
    }

    // it is possible that the mapper is not set yet even though not new - if the node is from a previous VB and
    // we have not yet encountered in node in this VB
    *ENT (SubfieldMapper, vb->format_mapper_buf, node_index) = format_mapper;
}

static void seg_info_field (VBlockVCF *vb, ZipDataLineVCF *dl, char *info_str, unsigned info_len, 
                            bool has_13, // this VCF file line ends with a Windows-style \r\n
                            unsigned vb_line_i)
{
    #define MAX_INFO_NAMES_LEN 1000 // max len of just the names string, without the data eg "INFO1=INFO2=INFO3="
    char iname[MAX_INFO_NAMES_LEN];
    unsigned iname_len = 0;
    const char *this_name = info_str;
    unsigned this_name_len = 0;
    const char *this_value = NULL;
    unsigned this_value_len=0;
    unsigned sf_i=0;

    // if the VCF file line ends with \r\n when we add an artificial additional info subfield "#"
    // we know we have space for adding ":#" because the line as at least a "\r\n" appearing somewhere after the INFO field
    if (has_13) {
        if (info_len && info_str[info_len-1] != '=') // last subfield is has no value (and hence no '=') - add a ';'
            info_str[info_len++] = ';';
        info_str[info_len++] = '#';
    }
    
    // count infos
    SubfieldMapper iname_mapper;
    memset (&iname_mapper, 0, sizeof (iname_mapper));

    for (unsigned i=0; i < info_len; i++) 
        if (info_str[i] == '=') iname_mapper.num_subfields++;
    
    // get name / value pairs - and insert values to the "name" dictionary
    bool reading_name = true;
    for (unsigned i=0; i < info_len + 1; i++) {
        char c = (i==info_len) ? ';' : info_str[i]; // add an artificial ; at the end of the INFO data

        if (reading_name) {
            iname[iname_len++] = c; // info names inc. the =. the = terminats each name
            ASSERT (iname_len <= MAX_INFO_NAMES_LEN, "Error: INFO field too long in vb_line_i=%u", vb_line_i);

            if (c == '=') {  // end of name

                ASSERT (this_name_len > 0, "Error: INFO field in vb_line_i=%u, contains a = without a preceding subfield name", vb_line_i);

                ASSERT (this_name[0] >= 64 && this_name[0] <= 127, 
                        "Error: INFO field in vb_line_i=%u, contains a name %.*s starting with an illegal character", vb_line_i, this_name_len, this_name);

                reading_name = false; 
                this_value = &info_str[i+1]; 
                this_value_len = 0;
            }

            // name without value - valid in VCF format
            else if (c == ';') {
                if (i==info_len) { // our artificial ; terminator
                    iname_len--; // remove ;
                    vb->txt_section_bytes[SEC_VCF_INFO_B250]++; // account for the separator (; or \t or \n)
                }
                else {
                    this_name = &info_str[i+1]; // skip the value-less name
                    this_name_len = 0;
                }
                continue;
            }
            
            else this_name_len++; // don't count the = or ; in the len
        }
        else {
            if (c == ';') { // end of value

                ASSERT (this_value_len > 0, 
                        "Error: INFO field in vb_line_i=%u, subfield %.*s, does not contain a value", vb_line_i, this_name_len, this_name);

                // find (or create) an MTF context (= a dictionary) for this name
                DictIdType dict_id = dict_id_vcf_info_sf (dict_id_make (this_name, this_name_len));

                // find which DictId (did_i) this subfield belongs to (+ create a new ctx if this is the first occurance)
                MtfContext *ctx = mtf_get_ctx_by_dict_id (vb->mtf_ctx, &vb->num_dict_ids, &vb->num_info_subfields, dict_id, SEC_VCF_INFO_SF_DICT);
                iname_mapper.did_i[sf_i] = ctx ? ctx->did_i : (uint8_t)NIL;

                // allocate memory if needed
                Buffer *mtf_i_buf = &ctx->mtf_i;
                buf_alloc (vb, mtf_i_buf, MIN (vb->num_lines, mtf_i_buf->len + 1) * sizeof (uint32_t),
                           1.5, "mtf_ctx->mtf_i", ctx->dict_section_type);

                MtfNode *sf_node;

                vb->txt_section_bytes[SEC_VCF_INFO_SF_B250] += this_value_len; 
                vb->txt_section_bytes[SEC_VCF_INFO_B250]++; // account for the separator (; or \t or \n) 

                unsigned optimized_snip_len;
                char optimized_snip[OPTIMIZE_MAX_SNIP_LEN];
                if (flag_optimize && (ctx->dict_id.num == dict_id_INFO_VQSLOD) &&
                    optimize_info (ctx->dict_id, this_value, this_value_len, optimized_snip, &optimized_snip_len)) {
                 
                    vb->vb_data_size -= (int)this_value_len - (int)optimized_snip_len;
                    this_value = optimized_snip;
                    this_value_len = optimized_snip_len;
                }

                NEXTENT (uint32_t, *mtf_i_buf) = 
                    mtf_evaluate_snip_seg ((VBlockP)vb, ctx, this_value, this_value_len, &sf_node, NULL);

                reading_name = true;  // end of value - move to the next time
                this_name = &info_str[i+1]; // move to next field in info string
                this_name_len = 0;
                sf_i++;
            }
            else  
                this_value_len++;
        }
    }

    // now insert the info names - a snip is a string that looks like: "INFO1=INFO2=INFO3="
    // 1. find it's mtf_i (and add to dictionary if a new name)
    // 2. place mtf_i in INFO section of this VB
    uint32_t *info_field_mtf_i = (uint32_t *)vb->mtf_ctx[VCF_INFO].mtf_i.data;
    MtfContext *info_ctx = &vb->mtf_ctx[VCF_INFO];
    MtfNode *node;
    bool is_new;
    uint32_t node_index = mtf_evaluate_snip_seg ((VBlockP)vb, info_ctx, iname, iname_len, &node, &is_new);
    info_field_mtf_i[vb->mtf_ctx[VCF_INFO].mtf_i.len++] = node_index;

    // if this is a totally new subfield (first time in this file) - make a new SubfieldMapper for it.
    if (is_new) {   
        ASSERT (node_index == vb->iname_mapper_buf.len, "Error: node_index=%u different than vb->iname_mapper_buf.len=%u", node_index, vb->iname_mapper_buf.len);
    
        vb->iname_mapper_buf.len++;
        buf_alloc (vb, &vb->iname_mapper_buf, MAX (100, vb->iname_mapper_buf.len) * sizeof (SubfieldMapper), 1.5, "iname_mapper_buf", 0);
    }

    // it is possible that the iname_mapper is not set yet even though not new - if the node is from a previous VB and
    // we have not yet encountered in node in this VB
    ((SubfieldMapper *)vb->iname_mapper_buf.data)[node_index] = iname_mapper;

    dl->info_mtf_i = node_index;

    vb->txt_section_bytes[SEC_VCF_INFO_B250] += iname_len; // this includes all the =
}

static void seg_increase_ploidy_one_line (VBlockVCF *vb, char *line_ht_data, unsigned new_ploidy, unsigned num_samples)
{
    // copy the haplotypes backwards (to avoid overlap), padding with '*'
    for (int sam_i = num_samples-1; sam_i >= 0; sam_i--) {

        int ht_i=new_ploidy-1 ; for (; ht_i >= vb->ploidy; ht_i--) 
            line_ht_data[sam_i * new_ploidy + ht_i] = '*';

        for (; ht_i >= 0; ht_i--)
            line_ht_data[sam_i * new_ploidy + ht_i] = line_ht_data[sam_i * vb->ploidy + ht_i];
    
        // note: no change in phase type of previous rows, even if they were PHASE_TYPE_HAPLO
    }
}

// increase ploidy of the previous lines, if higher ploidy was encountered
static void seg_increase_ploidy (VBlockVCF *vb, unsigned new_ploidy, unsigned vb_line_i, unsigned sample_i)
{
    // increase ploidy of all previous lines.
    for (unsigned i=0; i < vb_line_i; i++) {
        ZipDataLineVCF *dl = DATA_LINE (vb, i);

        char *old_haplotype_data = HAPLOTYPE_DATA(vb,dl);
        uint32_t old_ht_data_len = dl->haplotype_data_len;

        // abandon old allocation, and just re-allocate in the spillover area (not very memory-effecient, but this is hopefully a rare event)
        seg_store ((VBlockP)vb, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                   NULL, global_vcf_num_samples * new_ploidy, NULL, false);
        char *new_haplotype_data = HAPLOTYPE_DATA (vb, dl);
        
        if (old_haplotype_data) memcpy (new_haplotype_data, old_haplotype_data, old_ht_data_len);

        if (dl->has_haplotype_data)  // row already has haplotype (i.e. we're not increasing ploidy from 0, and not current row)
            seg_increase_ploidy_one_line (vb, new_haplotype_data, new_ploidy, global_vcf_num_samples);
    }

    // increase ploidy in all previous samples of this line (in the newly forming vb->line_ht_data, not dl)
    if (sample_i) seg_increase_ploidy_one_line (vb, vb->line_ht_data.data, new_ploidy, sample_i);

    vb->ploidy = new_ploidy;
}

static void seg_haplotype_area (VBlockVCF *vb, ZipDataLineVCF *dl, const char *str, unsigned len, unsigned vb_line_i, unsigned sample_i,
                                bool is_vcf_string)
{
    // check ploidy
    unsigned ploidy=1;
    for (unsigned i=1; i<len-1; i++)
        if (str[i] == '|' || str[i] == '/') ploidy++;

    if (is_vcf_string) {
        vb->txt_section_bytes[SEC_VCF_PHASE_DATA]     += ploidy-1;
        vb->txt_section_bytes[SEC_VCF_HT_DATA ] += ploidy;
    }

    ASSERT (ploidy <= MAX_PLOIDY, "Error: ploidy=%u exceeds the maximum of %u in vb_line_i=%u", ploidy, MAX_PLOIDY, vb_line_i);
    
    // if the ploidy of this line is bigger than the ploidy of the data in this VB so far, then
    // we have to increase ploidy of all the haplotypes read in in this VB so far. This can happen for example in 
    // the X chromosome if initial samples are male with ploidy=1 and then a female sample with ploidy=2
    if (vb->ploidy && ploidy > vb->ploidy) 
        seg_increase_ploidy (vb, ploidy, vb_line_i, sample_i);

    if (!vb->ploidy) vb->ploidy = ploidy; // very first sample in the vb

    // note - ploidy of this sample might be smaller than vb->ploidy (eg a male sample in an X chromosesome that was preceded by a female sample)
    if (sample_i == 0) {
        buf_alloc (vb, &vb->line_ht_data, vb->ploidy * global_vcf_num_samples, 1, "line_ht_data", vb_line_i);
        dl->phase_type = (vb->ploidy==1 ? PHASE_HAPLO : PHASE_UNKNOWN);
    }

    char *ht_data = &vb->line_ht_data.data[vb->ploidy * sample_i];

    PhaseType ht0_phase_type = PHASE_UNKNOWN;
    for (unsigned ht_i=0; ht_i < ploidy; ht_i++) {

        char ht = *(str++); 
        len--;

        ASSERT ((ht >= '0' && ht <= '9') || ht == '.' || ht == '*',
                "Error: invalid VCF file - vb_line_i=%u - expecting an allele in a sample to be a number 0-9 or . , but seeing %c", vb_line_i, *str);

        // single-digit allele numbers
        ht_data[ht_i] = ht;

        if (!len) break;

        // handle 2-digit allele numbers
        if (ht != '.' && *str >= '0' && *str <= '9') {
            unsigned allele = 10 * (ht-'0') + (*(str++) - '0');
            len--;

            // make sure there isn't a 3rd digit
            ASSERT (!len || *str < '0' || * str > '9', "Error: VCF file - vb_line_i=%u sample %u - genozip currently supports only alleles up to 99", vb_line_i, sample_i+1);

            ht_data[ht_i] = '0' + allele; // use ascii 48->147

            vb->txt_section_bytes[SEC_VCF_HT_DATA ]++;
        }

        // get phase
        if (ploidy > 1 && ht_i < ploidy-1) {
            
            PhaseType cell_phase_type = (PhaseType)*(str++);
            len--;

            ASSERT (cell_phase_type != ' ', "Error: invalid VCF file - vb_line_i=%u - expecting a tab or newline after sample %u but seeing a space", vb_line_i, sample_i+1);
            ASSERT (cell_phase_type == '|' || cell_phase_type == '/', "Error: invalid VCF file - vb_line_i=%u - unable to parse sample %u: expecting a | or / but seeing %c", vb_line_i, sample_i+1, cell_phase_type);

            // deal with phase - only at the first separator eg 1|1|0|1
            if (ht_i==0) { 
                if (cell_phase_type == dl->phase_type) {} // do nothing

                else if ((cell_phase_type == '|' || cell_phase_type == '/') && 
                         (dl->phase_type == PHASE_UNKNOWN || dl->phase_type == PHASE_HAPLO))
                    dl->phase_type = cell_phase_type;

                else if ((dl->phase_type == '|' && cell_phase_type == '/') || 
                         (dl->phase_type == '/' && cell_phase_type == '|')) {
                    dl->phase_type = PHASE_MIXED_PHASED;
                
                    buf_alloc (vb, &vb->line_phase_data, global_vcf_num_samples, 1, "line_phase_data", vb_line_i);

                    // fill in the so-far uniform phase (which is different than the current one)
                    memset (vb->line_phase_data.data, (cell_phase_type == '|' ? '/' : '|'), sample_i);
                    vb->line_phase_data.data[sample_i] = (char)cell_phase_type;
                }
                else if (dl->phase_type == PHASE_MIXED_PHASED)
                    vb->line_phase_data.data[sample_i] = (char)cell_phase_type;

                ht0_phase_type = cell_phase_type;
            }
            // subsequent seperators - only make sure they're consistent
            else
                ASSERT (cell_phase_type==ht0_phase_type, "Error: invalid VCF file - vb_line_i=%u - unable to parse sample %u: inconsistent phasing symbol '|' '/'", vb_line_i, sample_i+1);
        }
    } // for characters in a sample

    // if the ploidy of the sample is lower than vb->ploidy, set missing ht as '*' ("ploidy padding")
    if (ploidy != vb->ploidy) {
        
        for (unsigned ht_i=ploidy; ht_i < vb->ploidy; ht_i++) 
            ht_data[ht_i] = '*';
    }

    if (ploidy==1 && vb->ploidy > 1 && dl->phase_type == PHASE_MIXED_PHASED)
        vb->line_phase_data.data[sample_i] = (char)PHASE_HAPLO;
}

// returns length of the snip, not including the separator
static inline unsigned seg_snip_len_tnc (const char *snip, bool *has_13)
{
    const char *s; for (s=snip ; *s != '\t' && *s != ':' && *s != '\n'; s++);
    
    // check if we have a Windows-style \r\n line ending
    if (*s == '\n' && s > snip && s[-1] == '\r') {
        *has_13 = true;
        return s - snip - 1;
    }
    else
        return s - snip;
}

static int seg_genotype_area (VBlockVCF *vb, ZipDataLineVCF *dl, 
                              const char *cell_gt_data, 
                              unsigned cell_gt_data_len,  // not including the \t or \n 
                              unsigned vb_line_i,
                              bool is_vcf_string,
                              bool *has_13) // out (modified only if found \r)
{
    uint32_t *next = (uint32_t *)(&vb->line_gt_data.data[vb->line_gt_data.len]);
    SubfieldMapper *format_mapper = ENT (SubfieldMapper, vb->format_mapper_buf, dl->format_mtf_i);
    
    int optimized_cell_gt_data_len = cell_gt_data_len;

    bool end_of_cell = !cell_gt_data_len;
    for (unsigned sf=0; sf < format_mapper->num_subfields; sf++) { // iterate on the order as in the line

        // move next to the beginning of the subfield data, if there is any
        unsigned len = end_of_cell ? 0 : seg_snip_len_tnc (cell_gt_data, has_13);
        MtfContext *ctx = MAPPER_CTX (format_mapper, sf);

        MtfNode *node;
        uint32_t node_index;
        unsigned optimized_snip_len;
        char optimized_snip[OPTIMIZE_MAX_SNIP_LEN];

        if (flag_optimize && cell_gt_data && len && 
            (ctx->dict_id.num == dict_id_FORMAT_PL || ctx->dict_id.num == dict_id_FORMAT_GL || ctx->dict_id.num == dict_id_FORMAT_GP) && 
            optimize_format (ctx->dict_id, cell_gt_data, len, optimized_snip, &optimized_snip_len)) {

            node_index = mtf_evaluate_snip_seg ((VBlockP)vb, ctx, optimized_snip, optimized_snip_len, &node, NULL);
            vb->vb_data_size -= (int)len - (int)optimized_snip_len;
            optimized_cell_gt_data_len -= (int)len - (int)optimized_snip_len;
        }
        else 
            node_index = mtf_evaluate_snip_seg ((VBlockP)vb, ctx, cell_gt_data, len, &node, NULL);

        *(next++) = node_index;

        if (node_index != WORD_INDEX_MISSING_SF) // don't skip the \t if we might have more missing subfields
            cell_gt_data += len + 1 + *has_13; // skip separator too

        end_of_cell = end_of_cell || cell_gt_data[-1] != ':'; // a \t or \n encountered
    }
    ASSERT0 (end_of_cell, "Error: invalid reading of genotype data");

    vb->line_gt_data.len += format_mapper->num_subfields * sizeof(uint32_t);  // len is number of bytes

    if (is_vcf_string)
        // size including : (if we have both ht and gt), but not including \t which goes into SEC_STATS_HT_SEPERATOR
        vb->txt_section_bytes[SEC_VCF_GT_DATA] += cell_gt_data_len + (dl->has_haplotype_data && dl->has_genotype_data);

    return optimized_cell_gt_data_len;
}

// in some real-world files I encountered have too-short lines due to human errors. we pad them
static void seg_add_samples_missing_in_line (VBlockVCF *vb, ZipDataLineVCF *dl, unsigned *gt_line_len, 
                                             unsigned num_samples, unsigned vb_line_i)
{
    ASSERTW (false, "Warning: the number of samples in vb_line_i=%u is %u, different than the VCF column header line which has %u samples",
             vb_line_i, num_samples, global_vcf_num_samples);

    for (; num_samples < global_vcf_num_samples; num_samples++) {

        if (dl->has_haplotype_data) {
            // '*' (haplotype padding) with ploidy 1
            seg_haplotype_area (vb, dl, "*", 1, vb_line_i, num_samples, false);
        }

        if (dl->has_genotype_data) {
            bool has_13;
            seg_genotype_area (vb, dl, NULL, 0, vb_line_i, false, &has_13);
            (*gt_line_len)++; // adding the WORD_INDEX_MISSING_SF
        }
    }
}

static void seg_vcf_update_vb_from_dl (VBlockVCF *vb, ZipDataLineVCF *dl)
{
    // update block data
    vb->has_genotype_data = vb->has_genotype_data || dl->has_genotype_data;

    vb->has_haplotype_data = vb->has_haplotype_data || dl->has_haplotype_data;
    
    if (vb->phase_type == PHASE_UNKNOWN) 
        vb->phase_type = dl->phase_type;
    
    else if ((vb->phase_type == PHASE_PHASED     && dl->phase_type == PHASE_NOT_PHASED) ||
             (vb->phase_type == PHASE_NOT_PHASED && dl->phase_type == PHASE_PHASED) ||
              dl->phase_type  == PHASE_MIXED_PHASED)
        vb->phase_type = PHASE_MIXED_PHASED;    
}


/* split the data line into sections - 
   1. variant data - each of 9 fields (CHROM to FORMAT) is a section
   2. genotype data (except the GT subfield) - is a section
   3. haplotype data (the GT subfield) - a string of eg 1 or 0 (no whitespace) - ordered by the permutation order
   4. phase data - only if MIXED phase - a string of | and / - one per sample
*/
const char *seg_vcf_data_line (VBlock *vb_,   
                               const char *field_start_line,     // index in vb->txt_data where this line starts
                               uint32_t vb_line_i) // line within this vb (starting from 0)
{
    VBlockVCF *vb = (VBlockVCF *)vb_;
    ZipDataLineVCF *dl = DATA_LINE (vb, vb_line_i);

    dl->phase_type = PHASE_UNKNOWN;

    unsigned sample_i = 0;
    unsigned gt_line_len=0;

    const char *next_field, *field_start;
    unsigned field_len=0;
    char separator;
    bool has_13 = false; // does this line end in Windows-style \r\n rather than Unix-style \n

    int32_t len = &vb->txt_data.data[vb->txt_data.len] - field_start_line;

    // CHROM
    field_start = field_start_line;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "CHROM");
    seg_chrom_field (vb, field_start, field_len, vb_line_i);

    // POS
    field_start = next_field;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "POS");
    vb->last_pos = seg_pos_field (vb_, vb->last_pos, VCF_POS, SEC_VCF_POS_B250, field_start, field_len, vb_line_i);
    random_access_update_pos (vb, vb->last_pos);

    // ID
    field_start = next_field;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "ID");
    seg_vcf_one_field (vb, field_start, field_len, vb_line_i, VCF_ID, NULL);

    // REF + ALT
    // note: we treat REF+\t+ALT as a single field because REF and ALT are highly corrected, in the case of SNPs:
    // e.g. GG has a probability of 0 and GC has a higher probability than GA.
    field_start = next_field;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "REF");

    unsigned alt_len=0;
    next_field = seg_get_next_item (next_field, &len, false, true, false, vb_line_i, &alt_len, &separator, &has_13, "ALT");
    seg_vcf_one_field (vb, field_start, field_len+alt_len+1, vb_line_i, VCF_REFALT, NULL);

    // QUAL
    field_start = next_field;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "QUAL");
    seg_vcf_one_field (vb, field_start, field_len, vb_line_i, VCF_QUAL, NULL);

    // FILTER
    field_start = next_field;
    next_field = seg_get_next_item (field_start, &len, false, true, false, vb_line_i, &field_len, &separator, &has_13, "FILTER");
    seg_vcf_one_field (vb, field_start, field_len, vb_line_i, VCF_FILTER, NULL);

    // INFO
    char *info_field_start = (char *)next_field; // we break the const bc seg_info_field might add a :#
    unsigned info_field_len=0;
    next_field = seg_get_next_item (info_field_start, &len, global_vcf_num_samples==0, global_vcf_num_samples>0, false, vb_line_i, &info_field_len, &separator, &has_13, "INFO");
    // note: we delay seg_info_field() until the end of the line - we might be adding a Windows \r subfield

    if (separator != '\n') {

        // FORMAT
        field_start = next_field;
        next_field = seg_get_next_item (field_start, &len, true, true, false, vb_line_i, &field_len, &separator, &has_13, "FORMAT");
        seg_format_field (vb, dl, field_start, field_len, vb_line_i);

        ASSERT (separator == '\n' || dl->has_genotype_data || dl->has_haplotype_data, 
                "Error: expecting line vb_line_i=%u to end as it has no genotype or haplotype data, but it is not", vb_line_i);

        // 0 or more samples
        while (separator != '\n') {
            
            // get haplotype data
            bool has_genotype_data = dl->has_genotype_data;
            if (dl->has_haplotype_data) { // FORMAT declares GT, we may have it or not

                field_start = next_field;
                next_field = seg_get_next_item (field_start, &len, true, true, dl->has_genotype_data, vb_line_i, &field_len, &separator, &has_13, "GT");
                seg_haplotype_area (vb, dl, field_start, field_len, vb_line_i, sample_i, true);

                if (separator != ':' && has_genotype_data) {
                    // missing genotype data despite being declared in FORMAT - this is permitted by VCF spec
                    has_genotype_data = false;
                    bool has_13;
                    seg_genotype_area (vb, dl, NULL, 0, vb_line_i, false, &has_13);
                    gt_line_len++; // adding the WORD_INDEX_MISSING_SF
                }
            }

            if (has_genotype_data) { // FORMAT declares other subfields, we may have them or not
                field_start = next_field;
                next_field = seg_get_next_item (field_start, &len, true, true, false, vb_line_i, &field_len, &separator, &has_13, "Non-GT");

                ASSERT (field_len, "Error: invalid VCF file - expecting sample data for sample # %u on vb_line_i=%u, but found a tab character", 
                        sample_i+1, vb_line_i);

                // note: length can change as a result of optimize()
                unsigned updated_field_len = seg_genotype_area (vb, dl, field_start, field_len, vb_line_i, true, &has_13);
                gt_line_len += updated_field_len + 1; // including the \t or \n
            }

            sample_i++;

            vb->txt_section_bytes[SEC_STATS_HT_SEPERATOR]++; // the \t or \n following a sample

            ASSERT (sample_i < global_vcf_num_samples || separator == '\n',
                    "Error: invalid VCF file - expecting a newline after the last sample (sample #%u) on line %u", global_vcf_num_samples, vb_line_i);
        }
    }

    vb->txt_section_bytes[SEC_STATS_HT_SEPERATOR] -= has_13; // the \r in case of Windows \r\n line ending (WHY IS THIS?)

    // in some real-world files I encountered have too-short lines due to human errors. we pad them
    if (sample_i < global_vcf_num_samples) 
        seg_add_samples_missing_in_line (vb, dl, &gt_line_len, sample_i, vb_line_i);

    // update lengths
    if (dl->has_haplotype_data) {
        vb->line_ht_data.len = global_vcf_num_samples * vb->ploidy;

        if (dl->phase_type == PHASE_MIXED_PHASED) 
            vb->line_phase_data.len = global_vcf_num_samples;
    } else 
        vb->line_ht_data.len = 0;

    vb->max_gt_line_len = MAX (vb->max_gt_line_len, gt_line_len);

    // now do the info field - possibly with the added \r for Windows 
    seg_info_field (vb, dl, info_field_start, info_field_len, has_13, vb_line_i);

    // we don't need txt_data anymore, until the point we read - so we can overlay - if we have space. If not, we allocate use txt_data_spillover
    if (dl->has_genotype_data) 
        seg_store (vb_, &dl->genotype_data_spillover, &dl->genotype_data_start, &dl->genotype_data_len,
                   &vb->line_gt_data, 0, next_field, true);

    if (dl->has_haplotype_data && dl->phase_type == PHASE_MIXED_PHASED)
        seg_store (vb_, &dl->phase_data_spillover, &dl->phase_data_start, &dl->phase_data_len,
                   &vb->line_phase_data, 0, next_field, false);

    if (dl->has_haplotype_data) {
        seg_store (vb_, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                   &vb->line_ht_data, 0, next_field, false);
        
        if (flag_show_alleles) printf ("%.*s\n", dl->haplotype_data_len, HAPLOTYPE_DATA(vb,dl));
    }

    seg_vcf_update_vb_from_dl (vb, dl);

    return next_field;
}


// complete haplotype and genotypes of lines that don't have them, if we found out that some
// other line has them, and hence all lines in the vb must have them
void seg_vcf_complete_missing_lines (VBlockVCF *vb)
{
    vb->num_haplotypes_per_line = vb->ploidy * global_vcf_num_samples;
    const char *limit_txt_data = vb->txt_data.data + vb->txt_data.len;

    for (unsigned vb_line_i=0; vb_line_i < vb->num_lines; vb_line_i++) {

        ZipDataLineVCF *dl = DATA_LINE (vb, vb_line_i);

        if (vb->has_haplotype_data && !dl->has_haplotype_data) {
            seg_store ((VBlockP)vb, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                       NULL, vb->num_haplotypes_per_line, limit_txt_data, false);

            char *haplotype_data = HAPLOTYPE_DATA(vb,dl);
            memset (haplotype_data, '-', vb->num_haplotypes_per_line); // '-' means missing haplotype - note: ascii 45 (haplotype values start at ascii 48)

            // NOTE: we DONT set dl->has_haplotype_data to true bc downstream we still
            // count this row as having no GT field when analyzing gt data
        }

        if (vb->has_genotype_data && !dl->has_genotype_data) {
            seg_store ((VBlockP)vb, &dl->genotype_data_spillover, &dl->genotype_data_start, &dl->genotype_data_len, 
                       NULL, global_vcf_num_samples * sizeof(uint32_t), limit_txt_data, true);

            uint32_t *genotype_data = (uint32_t *)GENOTYPE_DATA(vb, dl);
            for (uint32_t i=0; i < global_vcf_num_samples; i++) 
                genotype_data[i] = WORD_INDEX_MISSING_SF;
        }
    }
}