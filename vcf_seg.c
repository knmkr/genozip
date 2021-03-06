// ------------------------------------------------------------------
//   vcf_seg.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <math.h>
#include "vcf_private.h"
#include "seg.h"
#include "move_to_front.h"
#include "random_access.h"
#include "optimize.h"
#include "file.h"
#include "strings.h"
#include "zip.h"
#include "dict_id.h"

#define DATA_LINE(i) ENT (ZipDataLineVCF, vb->lines, i)

// called from seg_all_data_lines
void vcf_seg_initialize (VBlock *vb_)
{
    VBlockVCF *vb = (VBlockVCF *)vb_;

    // initalize variant block data (everything else is initialzed to 0 via calloc)
    vb->phase_type = PHASE_UNKNOWN;  // phase type of this block
    vb->num_samples_per_block = global_vcf_samples_per_block;
    vb->num_sample_blocks = ceil((float)global_vcf_num_samples / (float)vb->num_samples_per_block);

    seg_init_mapper (vb_, VCF_FORMAT, &((VBlockVCF *)vb)->format_mapper_buf, "format_mapper_buf");    

    vb->contexts[VCF_CHROM] .flags = CTX_FL_NO_STONS; // needs b250 node_index for random access
    vb->contexts[VCF_FORMAT].flags = CTX_FL_NO_STONS;
    vb->contexts[VCF_INFO]  .flags = CTX_FL_NO_STONS;
}             

// traverses the FORMAT field, gets ID of subfield, and moves to the next subfield
static DictIdType vcf_seg_get_format_subfield (const char **str, uint32_t *len) // remaining length of line 
{
    unsigned i=0; for (; i < *len && (*str)[i] != ':' && (*str)[i] != '\t' && (*str)[i] != '\n'; i++);

    DictIdType dict_id = dict_id_vcf_format_sf (dict_id_make (*str, i));

    *str += i+1;
    *len -= i+1;
    return dict_id; 
}

static void vcf_seg_format_field (VBlockVCF *vb, ZipDataLineVCF *dl, 
                                  const char *field_start, int field_len)
{
    const char *str = field_start;
    int len = field_len;
    SubfieldMapper format_mapper;
    memset (&format_mapper, 0, sizeof (format_mapper));

    if (!global_vcf_num_samples) return; // if we're not expecting any samples, we ignore the FORMAT field

    ASSSEG0 (field_len, field_start, "Error: missing FORMAT field");

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
            ASSSEG (format_mapper.num_subfields < MAX_SUBFIELDS, field_start,
                    "Error: FORMAT field has too many subfields, the maximum allowed is %u (excluding GT)",  MAX_SUBFIELDS);

            DictIdType subfield = vcf_seg_get_format_subfield (&str, (unsigned *)&len);

            ASSSEG (dict_id_is_vcf_format_sf (subfield), field_start,
                    "Error: string %.*s in the FORMAT field is not a legal subfield", DICT_ID_LEN, subfield.id);

            MtfContext *ctx = mtf_get_ctx (vb, subfield);
            
            format_mapper.did_i[format_mapper.num_subfields++] = ctx ? ctx->did_i : (uint8_t)NIL;
        } 
        while (str[-1] != '\t' && str[-1] != '\n' && len > 0);
    }

    if (dl->has_genotype_data)
        buf_alloc (vb, &vb->line_gt_data, format_mapper.num_subfields * global_vcf_num_samples * sizeof(uint32_t), 1, "line_gt_data", vb->line_i); 

    bool is_new;
    uint32_t node_index = seg_by_did_i_ex (vb, field_start, field_len, VCF_FORMAT, field_len + 1 /* \t or \n */, &is_new);

    dl->format_mtf_i = node_index;

    // if this is a new format - add mapper
    if (is_new) {
        ASSERT (node_index == vb->format_mapper_buf.len, 
                "Error: node_index=%u different than vb->format_mapper_buf.len=%u", node_index, (uint32_t)vb->format_mapper_buf.len);

        vb->format_mapper_buf.len++;
        buf_alloc (vb, &vb->format_mapper_buf, vb->format_mapper_buf.len * sizeof (SubfieldMapper), 2, "format_mapper_buf", 0);
    }

    // it is possible that the mapper is not set yet even though not new - if the node is from a previous VB and
    // we have not yet encountered in node in this VB
    *ENT (SubfieldMapper, vb->format_mapper_buf, node_index) = format_mapper;
}

// store src_buf in dst_buf, and frees src_buf. we attempt to "allocate" dst_buf using memory from txt_data,
// but in the part of txt_data has been already consumed and no longer needed.
// if there's not enough space in txt_data, we allocate on txt_data_spillover
static void vcf_seg_store (VBlock *vb, 
                           bool *dst_is_spillover, uint32_t *dst_start, uint32_t *dst_len, // out
                           Buffer *src_buf, uint32_t size, // Either src_buf OR size must be given
                           const char *limit_txt_data, // we cannot store in txt starting here. if NULL always allocates in txt_data_spillover
                           bool align32) // does start address need to be 32bit aligned to prevent aliasing issues
{
    if (src_buf) size = src_buf->len;

    // align to 32bit (4 bytes) if needed
    if (align32 && (vb->txt_data_next_offset % 4))
        vb->txt_data_next_offset += 4 - (vb->txt_data_next_offset % 4);

    if (limit_txt_data && (vb->txt_data.data + vb->txt_data_next_offset + size < limit_txt_data)) { // we have space in txt_data - we can overlay
        *dst_is_spillover = false;
        *dst_start        = vb->txt_data_next_offset;
        *dst_len          = size;
        if (src_buf) memcpy (&vb->txt_data.data[*dst_start], src_buf->data, size);

        vb->txt_data_next_offset += size;
    }

    else {
        *dst_is_spillover = true;
        *dst_start = vb->txt_data_spillover.len;
        *dst_len = size;
        
        vb->txt_data_spillover.len += size;
        buf_alloc (vb, &vb->txt_data_spillover, MAX (1000, vb->txt_data_spillover.len), 1.5, 
                   "txt_data_spillover", vb->vblock_i);

        if (src_buf) memcpy (&vb->txt_data_spillover.data[*dst_start], src_buf->data, size);
    }

    if (src_buf) buf_free (src_buf);
}


static bool vcf_seg_special_info_subfields(VBlockP vb_, DictIdType dict_id, 
                                           const char **this_value, unsigned *this_value_len, char *optimized_snip)
{
    VBlockVCF *vb = (VBlockVCF *)vb_;
    unsigned optimized_snip_len;

    // Optimize VQSLOD
    if (flag_optimize_VQSLOD && (dict_id.num == dict_id_INFO_VQSLOD) &&
        optimize_float_2_sig_dig (*this_value, *this_value_len, 0, optimized_snip, &optimized_snip_len)) {
        
        vb->vb_data_size -= (int)(*this_value_len) - (int)optimized_snip_len;
        *this_value = optimized_snip;
        *this_value_len = optimized_snip_len;
        return true; // procede with adding to dictionary/b250
    }

    // POS and END share the same delta stream - the next POS will be a delta vs this END)
    if (dict_id.num == dict_id_INFO_END) {
        seg_pos_field ((VBlockP)vb, VCF_POS, VCF_POS, true, *this_value, *this_value_len, false); // END is an alias of POS
        return false; // do not add to dictionary/b250 - we already did it
    }

    return true; // all other cases -  procedue with adding to dictionary/b250
}

static void vcf_seg_increase_ploidy_one_line (VBlockVCF *vb, char *line_ht_data, unsigned new_ploidy, unsigned num_samples)
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
static void vcf_seg_increase_ploidy (VBlockVCF *vb, unsigned new_ploidy, unsigned sample_i)
{
    // increase ploidy of all previous lines.
    for (unsigned i=0; i < vb->line_i; i++) {
        ZipDataLineVCF *dl = DATA_LINE (i);

        char *old_haplotype_data = HAPLOTYPE_DATA(vb,dl);
        uint32_t old_ht_data_len = dl->haplotype_data_len;

        // abandon old allocation, and just re-allocate in the spillover area (not very memory-effecient, but this is hopefully a rare event)
        vcf_seg_store ((VBlockP)vb, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                   NULL, global_vcf_num_samples * new_ploidy, NULL, false);
        char *new_haplotype_data = HAPLOTYPE_DATA (vb, dl);
        
        if (old_haplotype_data) memcpy (new_haplotype_data, old_haplotype_data, old_ht_data_len);

        if (dl->has_haplotype_data)  // row already has haplotype (i.e. we're not increasing ploidy from 0, and not current row)
            vcf_seg_increase_ploidy_one_line (vb, new_haplotype_data, new_ploidy, global_vcf_num_samples);
    }

    // increase ploidy in all previous samples of this line (in the newly forming vb->line_ht_data, not dl)
    if (sample_i) vcf_seg_increase_ploidy_one_line (vb, vb->line_ht_data.data, new_ploidy, sample_i);

    vb->ploidy = new_ploidy;
}

static void vcf_seg_haplotype_area (VBlockVCF *vb, ZipDataLineVCF *dl, const char *str, unsigned len, unsigned sample_i,
                                    uint32_t add_bytes)
{
    // check ploidy
    unsigned ploidy=1;
    for (unsigned i=1; i<len-1; i++)
        if (str[i] == '|' || str[i] == '/') ploidy++;

    if (add_bytes) vb->contexts[VCF_GT].txt_len += add_bytes; 

    ASSSEG (ploidy <= VCF_MAX_PLOIDY, str, "Error: ploidy=%u exceeds the maximum of %u", ploidy, VCF_MAX_PLOIDY);
    
    // if the ploidy of this line is bigger than the ploidy of the data in this VB so far, then
    // we have to increase ploidy of all the haplotypes read in in this VB so far. This can happen for example in 
    // the X chromosome if initial samples are male with ploidy=1 and then a female sample with ploidy=2
    if (vb->ploidy && ploidy > vb->ploidy) 
        vcf_seg_increase_ploidy (vb, ploidy, sample_i);

    if (!vb->ploidy) vb->ploidy = ploidy; // very first sample in the vb

    // note - ploidy of this sample might be smaller than vb->ploidy (eg a male sample in an X chromosesome that was preceded by a female sample)
    if (sample_i == 0) {
        buf_alloc (vb, &vb->line_ht_data, vb->ploidy * global_vcf_num_samples, 1, "line_ht_data", vb->line_i);
        dl->phase_type = (vb->ploidy==1 ? PHASE_HAPLO : PHASE_UNKNOWN);
    }

    char *ht_data = &vb->line_ht_data.data[vb->ploidy * sample_i];

    PhaseType ht0_phase_type = PHASE_UNKNOWN;
    for (unsigned ht_i=0; ht_i < ploidy; ht_i++) {

        char ht = *(str++); 
        len--;

        ASSSEG (IS_DIGIT(ht) || ht == '.' || ht == '*', str,
                "Error: invalid VCF file - expecting an allele in a sample to be a number 0-9 or . , but seeing %c", *str);

        // single-digit allele numbers
        ht_data[ht_i] = ht;

        if (!len) break;

        // handle 2-digit allele numbers
        if (ht != '.' && IS_DIGIT (*str)) {
            unsigned allele = 10 * (ht-'0') + (*(str++) - '0');
            len--;

            // make sure there isn't a 3rd digit
            ASSSEG (!len || !IS_DIGIT (*str), str, "Error: VCF file sample %u - genozip currently supports only alleles up to 99", sample_i+1);

            ht_data[ht_i] = '0' + allele; // use ascii 48->147
        }

        // get phase
        if (ploidy > 1 && ht_i < ploidy-1) {
            
            PhaseType cell_phase_type = (PhaseType)*(str++);
            len--;

            ASSSEG (cell_phase_type != ' ', str, "Error: invalid VCF file - expecting a tab or newline after sample %u but seeing a space", sample_i+1);
            ASSSEG (cell_phase_type == '|' || cell_phase_type == '/', str, "Error: invalid VCF file -  unable to parse sample %u: expecting a | or / but seeing %c", sample_i+1, cell_phase_type);

            // deal with phase - only at the first separator eg 1|1|0|1
            if (ht_i==0) { 
                if (cell_phase_type == dl->phase_type) {} // do nothing

                else if ((cell_phase_type == '|' || cell_phase_type == '/') && 
                         (dl->phase_type == PHASE_UNKNOWN || dl->phase_type == PHASE_HAPLO))
                    dl->phase_type = cell_phase_type;

                else if ((dl->phase_type == '|' && cell_phase_type == '/') || 
                         (dl->phase_type == '/' && cell_phase_type == '|')) {
                    dl->phase_type = PHASE_MIXED_PHASED;
                
                    buf_alloc (vb, &vb->line_phase_data, global_vcf_num_samples, 1, "line_phase_data", vb->line_i);

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
                ASSSEG (cell_phase_type==ht0_phase_type, str, "Error: invalid VCF file - unable to parse sample %u: inconsistent phasing symbol '|' '/'", sample_i+1);
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
    *has_13 = (*s == '\n' && s > snip && s[-1] == '\r');

    return s - snip - *has_13;
}

static int vcf_seg_genotype_area (VBlockVCF *vb, ZipDataLineVCF *dl, uint32_t sample_i,
                                  const char *cell_gt_data, 
                                  unsigned cell_gt_data_len,  // not including the \t or \n 
                                  bool is_vcf_string,
                                  bool *has_13)
{
    SubfieldMapper *format_mapper = ENT (SubfieldMapper, vb->format_mapper_buf, dl->format_mtf_i);
    
    int optimized_cell_gt_data_len = cell_gt_data_len;

    bool end_of_cell = !cell_gt_data_len;

    int32_t dp_value = 0;

    for (unsigned sf=0; sf < format_mapper->num_subfields; sf++) { // iterate on the order as in the line

        // move next to the beginning of the subfield data, if there is any
        unsigned len = end_of_cell ? 0 : seg_snip_len_tnc (cell_gt_data, has_13);
        MtfContext *ctx = MAPPER_CTX (format_mapper, sf);

        if (cell_gt_data && ctx->dict_id.num == dict_id_FORMAT_DP) {
            ctx->flags |= CTX_FL_STORE_VALUE; // this ctx is used a base for a delta 
            dp_value = atoi (cell_gt_data); // an integer terminated by : \t or \n
        }

        uint32_t node_index;
        unsigned optimized_snip_len;
        char optimized_snip[OPTIMIZE_MAX_SNIP_LEN];

#       define EVAL_OPTIMIZED { \
            node_index = mtf_evaluate_snip_seg ((VBlockP)vb, ctx, optimized_snip, optimized_snip_len, NULL); \
            vb->vb_data_size -= (int)len - (int)optimized_snip_len; \
            optimized_cell_gt_data_len -= (int)len - (int)optimized_snip_len;\
        }

        if      (flag_optimize_PL && cell_gt_data && len && ctx->dict_id.num == dict_id_FORMAT_PL && 
            optimize_vcf_pl (cell_gt_data, len, optimized_snip, &optimized_snip_len)) 
            EVAL_OPTIMIZED

        else if (flag_optimize_GL && cell_gt_data && len && ctx->dict_id.num == dict_id_FORMAT_GL &&
            optimize_vector_2_sig_dig (cell_gt_data, len, optimized_snip, &optimized_snip_len))
            EVAL_OPTIMIZED
    
        else if (flag_optimize_GP && cell_gt_data && len && ctx->dict_id.num == dict_id_FORMAT_GP && 
            optimize_vector_2_sig_dig (cell_gt_data, len, optimized_snip, &optimized_snip_len))
            EVAL_OPTIMIZED

        // if case MIN_DP subfield - it is slightly smaller and usually equal to DP - we store MIN_DP as the delta DP-MIN_DP
        // note: the delta is vs. the DP field that preceeds MIN_DP - we take the DP as 0 there is no DP that preceeds
        else if (cell_gt_data && len && ctx->dict_id.num == dict_id_FORMAT_MIN_DP) {
            int32_t min_dp_value = atoi (cell_gt_data); // an integer terminated by : \t or \n
            int32_t delta = dp_value - min_dp_value; // expected to be 0 or positive integer (may be negative if no DP preceeds)
            char delta_str[30];
            unsigned delta_str_len = str_int (delta, delta_str);
            node_index = mtf_evaluate_snip_seg ((VBlockP)vb, ctx, delta_str, delta_str_len, NULL); 
            ctx->flags |= CTX_FL_NO_STONS;  /* currently handled uglyly by vcf_piz_reconstruct_genotype_data_line which doesn't allow lookup */ 
        }

        else
            node_index = mtf_evaluate_snip_seg ((VBlockP)vb, ctx, cell_gt_data, len, NULL);

        NEXTENT (uint32_t, vb->line_gt_data) = node_index;
        ctx->mtf_i.len++; // mtf_i is not used in FORMAT, we just update len for the use of stats_show_sections()
        
        if (is_vcf_string && len)
            ctx->txt_len += len + 1; // account for : \t or \n, but NOT \r (EOL accounts for it)

        if (node_index != WORD_INDEX_MISSING_SF) 
            cell_gt_data += len + 1 + *has_13; // skip separator too

        end_of_cell = end_of_cell || cell_gt_data[-1] != ':'; // a \t or \n encountered

        ASSSEG (!end_of_cell || !cell_gt_data || cell_gt_data[-1] == '\t' || cell_gt_data[-1] == '\n', &cell_gt_data[-1], 
                "Error in vcf_seg_genotype_area - end_of_cell and yet separator is %c (ASCII %u) is not \\t or \\n",
                cell_gt_data[-1], cell_gt_data[-1]);
    }
    ASSSEG0 (end_of_cell, cell_gt_data, "Error: More FORMAT subfields data than expected by the specification in the FORMAT field");

    return optimized_cell_gt_data_len;
}

// in some real-world files I encountered have too-short lines due to human errors. we pad them
static void vcf_seg_add_samples_missing_in_line (VBlockVCF *vb, ZipDataLineVCF *dl, unsigned *gt_line_len, 
                                                 unsigned sample_i)
{
    ASSERTW (false, "Warning: the number of samples in vb->line_i=%u is %u, different than the VCF column header line which has %u samples",
             vb->line_i, sample_i, global_vcf_num_samples);

    for (; sample_i < global_vcf_num_samples; sample_i++) {

        if (dl->has_haplotype_data) {
            // '*' (haplotype padding) with ploidy 1
            vcf_seg_haplotype_area (vb, dl, "*", 1, sample_i, 0);
        }

        if (dl->has_genotype_data) {
            bool has_13; 
            vcf_seg_genotype_area (vb, dl, sample_i, NULL, 0, false, &has_13);
            (*gt_line_len)++; // adding the WORD_INDEX_MISSING_SF
        }
    }
}

static void vcf_seg_update_vb_from_dl (VBlockVCF *vb, ZipDataLineVCF *dl)
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


// complete haplotype and genotypes of lines that don't have them, if we found out that some
// other line has them, and hence all lines in the vb must have them
void vcf_seg_complete_missing_lines (VBlockVCF *vb)
{
    vb->num_haplotypes_per_line = vb->ploidy * global_vcf_num_samples;
    const char *limit_txt_data = vb->txt_data.data + vb->txt_data.len;

    for (vb->line_i=0; vb->line_i < (uint32_t)vb->lines.len; vb->line_i++) {

        ZipDataLineVCF *dl = DATA_LINE (vb->line_i);

        if (vb->has_haplotype_data && !dl->has_haplotype_data) {
            vcf_seg_store ((VBlockP)vb, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                       NULL, vb->num_haplotypes_per_line, limit_txt_data, false);

            char *haplotype_data = HAPLOTYPE_DATA(vb,dl);
            memset (haplotype_data, '-', vb->num_haplotypes_per_line); // '-' means missing haplotype - note: ascii 45 (haplotype values start at ascii 48)

            // NOTE: we DONT set dl->has_haplotype_data to true bc downstream we still
            // count this row as having no GT field when analyzing gt data
        }

        if (vb->has_genotype_data && !dl->has_genotype_data) {
            vcf_seg_store ((VBlockP)vb, &dl->genotype_data_spillover, &dl->genotype_data_start, &dl->genotype_data_len, 
                       NULL, global_vcf_num_samples * sizeof(uint32_t), limit_txt_data, true);

            uint32_t *genotype_data = (uint32_t *)GENOTYPE_DATA(vb, dl);
            for (uint32_t i=0; i < global_vcf_num_samples; i++) 
                genotype_data[i] = WORD_INDEX_MISSING_SF;
        }
    }
}

/* split the data line into sections - 
   1. variant data - each of 9 fields (CHROM to FORMAT) is a section
   2. genotype data (except the GT subfield) - is a section
   3. haplotype data (the GT subfield) - a string of eg 1 or 0 (no whitespace) - ordered by the permutation order
   4. phase data - only if MIXED phase - a string of | and / - one per sample
*/
const char *vcf_seg_txt_line (VBlock *vb_, const char *field_start_line, bool *has_13)     // index in vb->txt_data where this line starts
{
    VBlockVCF *vb = (VBlockVCF *)vb_;
    ZipDataLineVCF *dl = DATA_LINE (vb->line_i);

    dl->phase_type = PHASE_UNKNOWN;

    uint32_t sample_i = 0;
    uint32_t gt_line_len=0;

    const char *next_field=field_start_line, *field_start;
    unsigned field_len=0;
    char separator;

    int32_t len = &vb->txt_data.data[vb->txt_data.len] - field_start_line;

    GET_NEXT_ITEM ("CHROM");
    seg_chrom_field (vb_, field_start, field_len);

    GET_NEXT_ITEM ("POS");
    seg_pos_field (vb_, VCF_POS, VCF_POS, false, field_start, field_len, true);
    random_access_update_pos (vb_, VCF_POS);

    GET_NEXT_ITEM ("ID");
    seg_id_field (vb_, (DictIdType)dict_id_fields[VCF_ID], field_start, field_len, true);

    // REF + ALT
    // note: we treat REF+\t+ALT as a single field because REF and ALT are highly corrected, in the case of SNPs:
    // e.g. GG has a probability of 0 and GC has a higher probability than GA.
    GET_NEXT_ITEM ("REF");
    unsigned alt_len=0;
    const char *alt_start = next_field;
    next_field = seg_get_next_item (vb, alt_start, &len, false, true, false, &alt_len, &separator, NULL, "ALT");
    seg_by_did_i (vb, field_start, field_len + alt_len + 1, VCF_REFALT, field_len + alt_len + 2);

    SEG_NEXT_ITEM (VCF_QUAL);
    SEG_NEXT_ITEM (VCF_FILTER);

    // INFO
    if (global_vcf_num_samples)
        GET_NEXT_ITEM (DTF(names)[VCF_INFO]) // pointer to string to allow pointer comparison 
    else
        GET_MAYBE_LAST_ITEM (DTF(names)[VCF_INFO]); // may or may not have a FORMAT field

    seg_info_field (vb_, vcf_seg_special_info_subfields, field_start, field_len, false);

    if (separator != '\n') { // has a FORMAT field

        // FORMAT
        GET_MAYBE_LAST_ITEM ("FORMAT");
        vcf_seg_format_field (vb, dl, field_start, field_len);

        ASSSEG0 (separator == '\n' || dl->has_genotype_data || dl->has_haplotype_data, field_start,
                "Error: expecting line to end as it has no genotype or haplotype data, but it is not");

        // 0 or more samples
        while (separator != '\n') {
            
            // get haplotype data
            bool has_genotype_data = dl->has_genotype_data;
            if (dl->has_haplotype_data) { // FORMAT declares GT, we may have it or not

                field_start = next_field;
                next_field = seg_get_next_item (vb, field_start, &len, true, true, dl->has_genotype_data, &field_len, &separator, has_13, "GT");
                vcf_seg_haplotype_area (vb, dl, field_start, field_len, sample_i, field_len + 1);

                if (separator != ':' && has_genotype_data) {
                    // missing genotype data despite being declared in FORMAT - this is permitted by VCF spec
                    has_genotype_data = false;
                    vcf_seg_genotype_area (vb, dl, sample_i, NULL, 0, false, has_13);
                    gt_line_len++; // adding the WORD_INDEX_MISSING_SF
                }
            }
            if (has_genotype_data) { // FORMAT declares other subfields, we may have them or not
                field_start = next_field;

                next_field = seg_get_next_item (vb, field_start, &len, true, true, false, &field_len, &separator, has_13, "Non-GT");

                ASSSEG (field_len, field_start, "Error: invalid VCF file - expecting sample data for sample # %u, but found a tab character", 
                        sample_i+1);

                // note: length can change as a result of optimize()
                unsigned updated_field_len = vcf_seg_genotype_area (vb, dl, sample_i, field_start, field_len, true, has_13);
                gt_line_len += updated_field_len + 1; // including the \t or \n
            }

            sample_i++;

            ASSSEG (sample_i < global_vcf_num_samples || separator == '\n', next_field,
                    "Error: invalid VCF file - expecting a newline after the last sample (sample #%u)", global_vcf_num_samples);
        }
    }

    SEG_EOL (VCF_EOL, false);

    // in some real-world files I encountered have too-short lines due to human errors. we pad them
    if (sample_i < global_vcf_num_samples) 
        vcf_seg_add_samples_missing_in_line (vb, dl, &gt_line_len, sample_i);

    // update lengths
    if (dl->has_haplotype_data) {
        vb->line_ht_data.len = global_vcf_num_samples * vb->ploidy;

        if (dl->phase_type == PHASE_MIXED_PHASED) 
            vb->line_phase_data.len = global_vcf_num_samples;
    } else 
        vb->line_ht_data.len = 0;

    vb->max_gt_line_len = MAX (vb->max_gt_line_len, gt_line_len);

    // we don't need txt_data anymore, until the point we read - so we can overlay - if we have space. If not, we allocate use txt_data_spillover
    if (dl->has_genotype_data) {
        vb->line_gt_data.len *= sizeof (uint32_t); // convert len to bytes
        vcf_seg_store (vb_, &dl->genotype_data_spillover, &dl->genotype_data_start, &dl->genotype_data_len,
                   &vb->line_gt_data, 0, next_field, true);
    }

    if (dl->has_haplotype_data && dl->phase_type == PHASE_MIXED_PHASED)
        vcf_seg_store (vb_, &dl->phase_data_spillover, &dl->phase_data_start, &dl->phase_data_len,
                   &vb->line_phase_data, 0, next_field, false);

    if (dl->has_haplotype_data) {
        vcf_seg_store (vb_, &dl->haplotype_data_spillover, &dl->haplotype_data_start, &dl->haplotype_data_len,
                   &vb->line_ht_data, 0, next_field, false);
        
        if (flag_show_alleles) printf ("%.*s\n", dl->haplotype_data_len, HAPLOTYPE_DATA(vb,dl));
    }

    vcf_seg_update_vb_from_dl (vb, dl);

    return next_field;
}
