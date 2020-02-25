// ------------------------------------------------------------------
//   zip.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <math.h>
#include "genozip.h"
#include "profiler.h"
#include "vb.h"
#include "buffer.h"
#include "file.h"
#include "zfile.h"
#include "vcffile.h"
#include "vcf_header.h"
#include "segregate.h"
#include "vb.h"
#include "dispatcher.h"
#include "move_to_front.h"
#include "zip.h"
#include "base250.h"

// read entire variant block to memory. this is called from the dispatcher thread
static void zip_read_variant_block (File *vcf_file,
                                    unsigned *line_i,   // in/out next line to be read
                                    Buffer *first_data_line,    // first line might be supplied by caller 
                                    VariantBlock *vb)
{
    unsigned first_line= *line_i;

    if (!vb->data_lines) 
        vb->data_lines = calloc (global_max_lines_per_vb, sizeof (DataLine));
    
    unsigned vb_line_i;
    vb->vb_data_size = 0; // size of variant block as it appears in the source file
    for (vb_line_i=0; vb_line_i < global_max_lines_per_vb; vb_line_i++) 
    {
        DataLine *dl = &vb->data_lines[vb_line_i];

        if (vb_line_i > 0 || !first_data_line) { // first line might be supplied by caller

            // allocate line Buffer and read line from file 
            bool success = vcffile_get_line (vb, first_line + vb_line_i, false, &dl->line, "dl->line"); 
            if (!success) break; // no more lines - we're done
        }
        else {
            buf_copy (vb, &dl->line, first_data_line, 1, 0, 0, "dl->line", vb->variant_block_i);
            buf_free (first_data_line); 
        }
        dl->line_i = first_line + vb_line_i;

        (*line_i)++;
        
        // count bytes in the source file, for sanity check after we reconstruct
        vb->vb_data_size += dl->line.len;
    }

    vb->num_lines = vb_line_i;
}

// here we translate the mtf_i indeces creating during seg_* to their finally dictionary indeces in base-250.
// Note that the dictionary indeces have changed since segregate (which is why we needed this intermediate step)
// because: 1. the dictionary got integrated into the global one - some values might have already been in the global
// dictionary thanks to other threads working on other VBs ; 2. for the first VB, we sort the dictionary by frequency
static void zip_generate_b250_section (VariantBlock *vb, MtfContext *ctx)
{
    buf_alloc (vb, &ctx->b250, ctx->mtf_i.len * MAX_BASE250_NUMERALS, // maximum length is if all entries are 5-numeral.
               1.1, "ctx->b250_buf", 0);

    ASSERT (ctx->encoding == BASE250_ENCODING_8BIT || ctx->encoding == BASE250_ENCODING_16BIT,
            "Error: invalid encoding %u in for dict_id=%.*s", ctx->encoding, DICT_ID_LEN, dict_id_printable (ctx->dict_id).id);

    bool show = flag_show_b250 || dict_id_printable (ctx->dict_id).num == dict_id_show_one_b250.num;

    if (show) printf ("vb_i=%u %.*s: ", vb->variant_block_i, DICT_ID_LEN, dict_id_printable(ctx->dict_id).id);

    int32_t prev = -1; 
    for (unsigned i=0; i < ctx->mtf_i.len; i++) {

        MtfNode *node = mtf_node (ctx, ((const uint32_t *)ctx->mtf_i.data)[i], NULL, NULL);
        
        Base250 index = node->word_index;

        bool one_up = (index.n == prev + 1) && (ctx->b250_section_type != SEC_GENOTYPE_DATA) && (i > 0);

        if (one_up) // note: we can't do SEC_GENOTYPE_DATA bc we can't PIZ it as many GT data types are in the same section 
            ctx->b250.data[ctx->b250.len++] = BASE250_ONE_UP;

        else if (index.num_numerals == 1)  // shortcut for most common case
            ctx->b250.data[ctx->b250.len++] = index.numerals[0];

        else {
            memcpy (&ctx->b250.data[ctx->b250.len], &index.numerals, index.num_numerals);
            ctx->b250.len += index.num_numerals;
        }

        if (show) {
            if (one_up) printf ("L%u:ONE_UP ", vb->first_line + i);
            else        printf ("L%u:%u ", vb->first_line + i, index.n);
        }

        prev = index.n;
    }
    if (show) printf ("\n");
}

#define SBL(line_i,sb_i) ((line_i) * vb->num_sample_blocks + (sb_i))

static unsigned zip_get_genotype_vb_start_len (VariantBlock *vb)
{
    buf_alloc (vb, &vb->genotype_section_lens_buf, sizeof(unsigned) * vb->num_sample_blocks, 1, "section_lens_buf", 0);
    unsigned section_0_len = 0; // all sections are the same length except the last that might be shorter

    // offsets into genotype data of individual lines
    buf_alloc (vb, &vb->gt_sb_line_starts_buf, 
               vb->num_lines * vb->num_sample_blocks * sizeof(uint32_t*), 
               0, "gt_sb_line_starts_buf", vb->first_line);
    uint32_t **gt_sb_line_starts = (uint32_t**)vb->gt_sb_line_starts_buf.data; 
    
    // each entry is the length of a single line in a sample block
    buf_alloc (vb, &vb->gt_sb_line_lengths_buf, 
               vb->num_lines * vb->num_sample_blocks * sizeof(unsigned), 
               0, "gt_sb_line_lengths_buf", vb->first_line);
    unsigned *gt_sb_line_lengths = (unsigned *)vb->gt_sb_line_lengths_buf.data; 
    
    // calculate offsets and lengths of genotype data of each sample block
    for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {

        uint32_t *gt_data  = (uint32_t*)vb->data_lines[line_i].genotype_data.data;
        
        unsigned format_mtf_i = vb->data_lines[line_i].format_mtf_i;
        SubfieldMapperZip *format_mapper = &((SubfieldMapperZip *)vb->format_mapper_buf.data)[format_mtf_i];
        
        for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

            unsigned num_samples_in_sb = vb_num_samples_in_sb (vb, sb_i);

            gt_sb_line_starts[SBL(line_i, sb_i)] = 
                &gt_data[SAMPLES_PER_BLOCK * sb_i * format_mapper->num_subfields];

            unsigned num_subfields_in_sample_line = format_mapper->num_subfields * num_samples_in_sb; // number of uint32_t
            gt_sb_line_lengths[SBL(line_i, sb_i)] = num_subfields_in_sample_line;

            if (!sb_i) section_0_len += num_subfields_in_sample_line;
        }
    }

    return section_0_len; // in subfields (uint32_t)
}

// split genotype data to sample groups, within a sample group genotypes are separated by a tab
static void zip_generate_genotype_one_section (VariantBlock *vb, unsigned sb_i)
{
    START_TIMER;

    // calculate encoding of each gt data context

    // build sample block genetype data
    uint8_t *dst_next = (uint8_t *)vb->genotype_one_section_data.data;
    
    // move the GT items from the line data to the permuted data - with each 
    // sample block of gt data containing the data in TRANSPOSED order - i.e. first
    // the gt data for all the variants for sample 1, then all of samples 2 etc.
    unsigned num_samples_in_sb = vb_num_samples_in_sb (vb, sb_i);
    for (unsigned sample_i=0; sample_i < num_samples_in_sb; sample_i++) {

        if (flag_show_gt_nodes) printf ("sample=%u (vb_i=%u sb_i=%u):\n", sb_i * SAMPLES_PER_BLOCK + sample_i + 1, vb->variant_block_i, sb_i);

        for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {

            if (flag_show_gt_nodes) printf ("  L%u: ", line_i + vb->first_line);

            DataLine *dl = &vb->data_lines[line_i];

            SubfieldMapperZip *format_mapper = &((SubfieldMapperZip *)vb->format_mapper_buf.data)[dl->format_mtf_i];

            unsigned **sb_lines = (uint32_t**)vb->gt_sb_line_starts_buf.data;
            unsigned *this_line = sb_lines[SBL(line_i, sb_i)];
            
            // lookup word indices in the global dictionary for all the subfields
            const uint8_t *dst_start = dst_next;

            // IS THIS A BUG? WE DON'T SEEM TO EVER USE THIS VARIABLE num_subfields
            int num_subfields = format_mapper->num_subfields;
            ASSERT (num_subfields >= 0, "Error: format_mapper->num_subfields=%d", format_mapper->num_subfields);

            // if this VB has subfields in some line, but not in this line, then we have filled it in seg_complete_missing_lines(), 
            // therefore we have 1 fake subfield
            if (vb->num_subfields > 0 && num_subfields==0) num_subfields = 1;

            for (unsigned sf=0; sf < format_mapper->num_subfields; sf++) { // iterate on the order as in the line
            
                uint32_t node_index = this_line[format_mapper->num_subfields * sample_i + sf];

                if (node_index <= WORD_INDEX_MAX_INDEX) { // normal index

                    MtfContext *ctx = format_mapper->ctx[sf];
                    MtfNode *node = mtf_node (ctx, node_index, NULL, NULL);
                    Base250 index = node->word_index;

                    if (flag_show_gt_nodes) printf ("%.*s:%u ", DICT_ID_LEN, dict_id_printable (ctx->dict_id).id, index.n);

                    if (index.num_numerals == 1) { // shortcut for most common case
                        *(dst_next++) = index.numerals[0];
                    }
                    else {
                        memcpy (dst_next, &index.numerals, index.num_numerals);
                        dst_next += index.num_numerals;
                    }
                }
                else if (node_index == WORD_INDEX_MISSING_SF) {
                    *(dst_next++) = BASE250_MISSING_SF;
                }
                else {  // node_index == WORD_INDEX_EMPTY_SF
                    *(dst_next++) = BASE250_EMPTY_SF;
                }
            }

            vb->genotype_one_section_data.len += dst_next - dst_start;
            
            if (flag_show_gt_nodes) printf ("\n");
        }
    }

    COPY_TIMER (vb->profile.zip_generate_genotype_sections)
}

// split phase data to sample groups, in each group is a string of / | or -
static void zip_generate_phase_sections (VariantBlock *vb)
{   
    START_TIMER;

    // we allocate memory for the Buffer array only once the first time this VariantBlock
    // is used. Subsequent blocks reusing the memory will have the same number of samples (by VCF spec)
    if (!vb->phase_sections_data) 
        vb->phase_sections_data = (Buffer *)calloc (vb->num_sample_blocks, sizeof(Buffer)); // allocate once, never free

    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

        unsigned num_samples_in_sb = vb_num_samples_in_sb (vb, sb_i); 
    
        // allocate memory for phase data for each sample block - one character per sample
        buf_alloc (vb, &vb->phase_sections_data[sb_i], vb->num_lines * num_samples_in_sb, 
                   0, "phase_sections_data", vb->first_line);

        // build sample block genetype data
        char *next = vb->phase_sections_data[sb_i].data;
        
        for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {
            
            DataLine *dl = &vb->data_lines[line_i];
            if (dl->phase_type == PHASE_MIXED_PHASED) 
                memcpy (next, &dl->phase_data.data[sb_i * num_samples_in_sb], num_samples_in_sb);
            else
                memset (next, (char)dl->phase_type, num_samples_in_sb);

            next += num_samples_in_sb;
        }
        vb->phase_sections_data[sb_i].len = num_samples_in_sb * vb->num_lines;
    }
          
    // add back the phase data bytes that weren't actually "saved"
    COPY_TIMER (vb->profile.zip_generate_phase_sections)
}

typedef struct {
    int num_alt_alleles;
    unsigned index_in_original_line;
    unsigned index_in_sorted_line;
} HaploTypeSortHelperIndex;

static int sort_by_alt_allele_comparator(const void *p, const void *q)  
{ 
    int l = ((HaploTypeSortHelperIndex *)p)->num_alt_alleles; 
    int r = ((HaploTypeSortHelperIndex *)q)->num_alt_alleles;  
    return (l - r); 
}

static int sort_by_original_index_comparator(const void *p, const void *q)  
{ 
    int l = ((HaploTypeSortHelperIndex *)p)->index_in_original_line; 
    int r = ((HaploTypeSortHelperIndex *)q)->index_in_original_line;  
    return (l - r); 
}

// sort haplogroups by alt allele count within the variant group, create an index for it, and split
// it to sample groups. for each sample a haplotype is just a string of 1 and 0 etc (could be other alleles too)
static void zip_generate_haplotype_sections (VariantBlock *vb)
{
    START_TIMER;

    // we allocate memory for the Buffer array only once the first time this VariantBlock
    // is used. Subsequent blocks reusing the memory will have the same number of samples (by VCF spec)
    if (!vb->haplotype_sections_data) 
        vb->haplotype_sections_data = (Buffer *)calloc (vb->num_sample_blocks, sizeof(Buffer)); // allocate once, never free

    // TO DO : there's a bug in this allocation ^ if a user is compressing multiple VCF, they 
    // may not have the same num_sample_blocks. We should remember how many we allocated and add more
    // if needed.
    
    // create a permutation index for the whole variant block, and permuted haplotypes for each sample block        
    buf_alloc (vb, &vb->haplotype_permutation_index, vb->num_haplotypes_per_line * sizeof(unsigned), 
               0, "haplotype_permutation_index", vb->first_line);

    buf_alloc (vb, &vb->helper_index_buf, vb->num_haplotypes_per_line * sizeof(HaploTypeSortHelperIndex), 0,
               "helper_index_buf", vb->first_line);
    buf_zero (&vb->helper_index_buf);
    HaploTypeSortHelperIndex *helper_index = (HaploTypeSortHelperIndex *)vb->helper_index_buf.data;

    { 
        START_TIMER; 
        // build index array 
        for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line; ht_i++) 
            helper_index[ht_i].index_in_original_line = ht_i;

        for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {
            for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line; ht_i++) {

                // we count as alt alleles : 1 - 99 (ascii 49 to 147)
                //             ref alleles : 0 . (unknown) - (missing) * (ploidy padding)
                char one_ht = vb->data_lines[line_i].haplotype_data.data[ht_i];
                if (one_ht >= '1')
                    helper_index[ht_i].num_alt_alleles++;
            }
        }
        COPY_TIMER (vb->profile.count_alt_alleles);
    }

    // sort the helper index array by number of alt alleles
    qsort (helper_index, vb->num_haplotypes_per_line, sizeof (HaploTypeSortHelperIndex), sort_by_alt_allele_comparator);

    // now build per-sample-block haplotype array, picking haplotypes by the order of the helper index array
    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

        unsigned num_haplotypes_in_sample_block = 
            vb->ploidy * vb_num_samples_in_sb (vb, sb_i); 

        // allocate memory for haplotype data for each sample block - one character per haplotype
        buf_alloc (vb, &vb->haplotype_sections_data[sb_i], vb->num_lines * num_haplotypes_in_sample_block, 
                   0, "haplotype_sections_data", vb->first_line);

        // build sample block haplptype data - 
        // -- using the helper index to access the haplotypes in sorted order
        // -- transposing the array
        char *next = vb->haplotype_sections_data[sb_i].data;
        
        {   // this loop, tested with 1KGP data, takes up to 1/5 of total compute time, so its highly optimized
            START_TIMER;
            unsigned helper_index_sb_i = sb_i * vb->num_samples_per_block * vb->ploidy;

            for (unsigned ht_i=0; ht_i < num_haplotypes_in_sample_block; ht_i++) {
                
                unsigned haplotype_data_char_i = helper_index[helper_index_sb_i + ht_i].index_in_original_line;
                char **ht_data_ptr = &vb->data_lines[0].haplotype_data.data; // this pointer moves sizeof(DataLine) bytes each iteration - i.e. to the exact same field in the next line

                for (unsigned line_i=0; line_i < vb->num_lines; line_i++, ht_data_ptr += sizeof(DataLine)/sizeof(char*)) 
                    *(next++) = (*ht_data_ptr)[haplotype_data_char_i];
            }
            COPY_TIMER (vb->profile.sample_haplotype_data);
        }
        vb->haplotype_sections_data[sb_i].len = num_haplotypes_in_sample_block * vb->num_lines;
    }

    // final step - build the reverse index that will allow access by the original index to the sorted array
    // this will be included in the genozip file
    for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line ; ht_i++)
        helper_index[ht_i].index_in_sorted_line = ht_i;

    // sort array back to its original order
    qsort (helper_index, vb->num_haplotypes_per_line, sizeof (HaploTypeSortHelperIndex), sort_by_original_index_comparator);

    // construct file index
    unsigned *hp_index = (unsigned *)vb->haplotype_permutation_index.data;
    for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line ; ht_i++)
        hp_index[ht_i] = helper_index[ht_i].index_in_sorted_line;

    buf_free (&vb->helper_index_buf);

    COPY_TIMER (vb->profile.zip_generate_haplotype_sections);
}

// this function receives all lines of a variant block and processes them
// in memory to the compressed format. This thread then terminates the I/O thread writes the output.
static void zip_compress_one_vb (VariantBlock *vb)
{ 
    START_TIMER;

    // allocate memory for the final compressed data of this vb. allocate 20% of the 
    // vb size on the original file - this is normally enough. if not, we will realloc downstream
    buf_alloc (vb, &vb->z_data, vb->vb_data_size / 5, 1.2, "z_data", 0);

    // initalize variant block data (everything else is initialzed to 0 via calloc)
    vb->phase_type = PHASE_UNKNOWN;  // phase type of this block
    vb->num_samples_per_block = SAMPLES_PER_BLOCK;
    vb->num_sample_blocks = ceil((float)global_num_samples / (float)vb->num_samples_per_block);

    // if testing, make a copy of the original lines read from the file, to compare to the result of vunblocking later
    Buffer *lines_orig = NULL;
    unsigned max_genotype_section_len=0; // length in subfields

    // clone global dictionaries while granted exclusive access to the global dictionaries
    mtf_clone_ctx (vb);

    // split each lines in this variant block to its components
    seg_all_data_lines (vb, lines_orig);

    // for the first vb only - sort dictionaries so that the most frequent entries get single digit
    // base-250 indices. This can be done only before any dictionary is written to disk, but likely
    // beneficial to all vbs as they are likely to more-or-less have the same frequent entries
    if (vb->variant_block_i == 1)
        mtf_sort_dictionaries_vb_1(vb);

    // if block has haplotypes - handle them now
    if (vb->has_haplotype_data)
        zip_generate_haplotype_sections (vb); 

    // if block has genetype data 
    if (vb->has_genotype_data) 
        // calculate starts, lengths and allocate memory
        max_genotype_section_len = zip_get_genotype_vb_start_len (vb); // length in subfields

    // if block has phase data - handle it
    if (vb->phase_type == PHASE_MIXED_PHASED) 
        zip_generate_phase_sections (vb);

    unsigned variant_data_header_pos = vb->z_data.len;
    zfile_compress_vb_header (vb); // variant data header + ht index

    // merge new words added in this vb into the z_file.mtf_ctx, ahead of zip_generate_b250_section() and
    // zip_generate_genotype_one_section(). writing indices based on the merged dictionaries. dictionaries are compressed. 
    // all this is done while holding exclusive access to the z_file dictionaries.
    // at the same time, we also merge in ra_buf (random access index) into z_file
    
    uint8_t field_dictionary_sections_bitmap;
    uint32_t num_info_dictionary_sections;
    uint32_t num_gt_dictionary_sections;
    mtf_merge_in_vb_ctx(vb, &field_dictionary_sections_bitmap, &num_info_dictionary_sections, &num_gt_dictionary_sections);

    // generate & write b250 data for all fields (CHROM to FORMAT)
    for (VcfFields f=CHROM ; f <= FORMAT ; f++) {
        MtfContext *ctx = &vb->mtf_ctx[f];
        zip_generate_b250_section (vb, ctx);
        zfile_compress_b250_data (vb, ctx);
    }

    // generate & write b250 data for all INFO subfields
    unsigned num_info_subfields=0;
    for (unsigned did_i=0; did_i < MAX_DICTS; did_i++) {
                
        MtfContext *ctx = &vb->mtf_ctx[did_i];
        if (ctx->dict_section_type != SEC_INFO_SUBFIELD_DICT) continue; // not an info subfield

        zip_generate_b250_section (vb, ctx);
        zfile_compress_b250_data (vb, ctx);
        num_info_subfields++;
    }
    ASSERT (num_info_subfields <= MAX_SUBFIELDS, "Error: vb_i=%u has %u INFO subfields, which exceeds the maximum of %u",
            vb->variant_block_i, num_info_subfields, MAX_SUBFIELDS);

    // compress the sample data - genotype, haplotype and phase sections. genotype data is generated here too.
    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {
        
        if (vb->has_genotype_data) {

            // in the worst case scenario, each subfield is represnted by 5 bytes in Base250
            // TO DO: this is a huge waste of memory, as normally each subfield consumes ~ 1.5 B, better allocate less and realloc
            buf_alloc (vb, &vb->genotype_one_section_data, max_genotype_section_len * 5, 1, "genotype_one_section_data", sb_i);

            // we compress each section at a time to save memory
            zip_generate_genotype_one_section (vb, sb_i); 

            zfile_compress_section_data (vb, SEC_GENOTYPE_DATA, &vb->genotype_one_section_data);

            buf_free (&vb->genotype_one_section_data);
        }

        if (vb->phase_type == PHASE_MIXED_PHASED)
            zfile_compress_section_data (vb, SEC_PHASE_DATA, &vb->phase_sections_data[sb_i]);

        if (vb->has_haplotype_data)
            zfile_compress_section_data (vb, SEC_HAPLOTYPE_DATA, &vb->haplotype_sections_data[sb_i]);
    }

    // update z_data in memory (its not written to disk yet)
    zfile_update_compressed_vb_header (vb, variant_data_header_pos, field_dictionary_sections_bitmap, 
                                       num_info_dictionary_sections, num_gt_dictionary_sections, num_info_subfields);
    
    COPY_TIMER (vb->profile.compute);

    vb->is_processed = true; // tell dispatcher this thread is done and can be joined. this operation needn't be atomic, but it likely is anyway
}

static void zip_output_processed_vb (VariantBlock *processed_vb, File *vcf_file, File *z_file, bool is_final)
{
    START_TIMER;
    file_write (z_file, processed_vb->z_data.data, processed_vb->z_data.len);
    COPY_TIMER (processed_vb->profile.write);

    z_file->disk_so_far      += (long long)processed_vb->z_data.len;
    z_file->vcf_data_so_far  += (long long)processed_vb->vb_data_size;
    z_file->num_lines_single += (long long)processed_vb->num_lines;
    z_file->num_lines_concat += (long long)processed_vb->num_lines;

    // update section stats
    for (unsigned sec_i=1; sec_i < NUM_SEC_TYPES; sec_i++) {
        vcf_file->section_bytes[sec_i]  += processed_vb->vcf_section_bytes[sec_i];
        z_file->num_sections[sec_i]     += processed_vb->z_num_sections[sec_i];
        z_file->section_bytes[sec_i]    += processed_vb->z_section_bytes[sec_i];
        z_file->section_entries[sec_i]  += processed_vb->z_section_entries[sec_i];
    }

    if (is_final) {

        ASSERT (!z_file->vcf_data_size_single || 
                z_file->vcf_data_size_single /* read from VCF file metadata */ == z_file->vcf_data_so_far, /* actually read */
                "Error: VCF file length inconsistency - read from VCF file metadata: %" PRIu64 " actually read: %" PRIu64 "",
                z_file->vcf_data_size_single, z_file->vcf_data_so_far);

        vcf_file->vcf_data_size_single = z_file->vcf_data_size_single = vcf_file->vcf_data_so_far;

        z_file->vcf_data_size_concat += z_file->vcf_data_so_far; // we completed one VCF file - add to the count
    }
}

void zip_wrap_up()
{

}

// this is the main dispatcher function. It first processes the VCF header, then proceeds to read 
// a variant block from the input file and send it off to a thread for computation. When the thread
// completes, this function proceeds to write the output to the output file. It can dispatch
// several threads in parallel.
void zip_dispatcher (const char *vcf_basename, File *vcf_file, 
                     File *z_file, bool test_mode, unsigned max_threads, bool is_last_file)
{
    static unsigned last_variant_block_i = 0; // used if we're concatenating files - the variant_block_i will continue from one file to the next
    if (!flag_concat_mode) last_variant_block_i = 0; // reset if we're not concatenating

    // normally max_threads would be the number of cores available - we allow up to this number of compute threads, 
    // because the I/O thread is normally idling waiting for the disk, so not consuming a lot of CPU
    Dispatcher dispatcher = dispatcher_init (max_threads, POOL_ID_ZIP, last_variant_block_i, vcf_file, z_file, test_mode, is_last_file, !flag_show_alleles, vcf_basename);

    VariantBlock *pseudo_vb = dispatcher_get_pseudo_vb (dispatcher);

    if (!z_file->disk_so_far) // write the genozip header only if this is the first vcf component
        zfile_write_genozip_header (pseudo_vb, DATA_TYPE_VCF, NULL, false);

    unsigned line_i = 0; // last line read (first line in file = 1, consistent with script line numbers)

    // first compress the VCF header
    Buffer *first_data_line = NULL; // contains a value only for first variant block, otherwise empty. 
    
    // read the vcf header, assign the global variables, and write the compressed header to the GENOZIP file
    off64_t vcf_header_header_pos = z_file->disk_so_far;
    bool success = vcf_header_vcf_to_genozip (pseudo_vb, &line_i, &first_data_line);
    if (!success) goto finish;

    mtf_initialize_mutex (z_file, last_variant_block_i+1);

    // this is the dispatcher loop. In each iteration, it can do one of 3 things, in this order of priority:
    // 1. In there is a new variant block avaialble, and a compute thread available to take it - dispatch it
    // 2. If there is no new variant block available, but input is not exhausted yet - read one
    // 3. Wait for the first thread (by sequential order) to complete the compute and output the results
    VariantBlock *next_vb;
    do {
        next_vb = dispatcher_get_next_vb (dispatcher);
        
        // PRIORITY 1: is there a block available and a compute thread available? in that case dispatch it
        if (next_vb && next_vb->ready_to_dispatch && dispatcher_has_free_thread (dispatcher)) 
            dispatcher_compute (dispatcher, zip_compress_one_vb);

        // PRIORITY 2: output completed vbs, so they can be released and re-used
        else if (dispatcher_has_processed_vb (dispatcher, NULL)) {
            VariantBlock *processed_vb = dispatcher_get_processed_vb (dispatcher, NULL);
            if (!processed_vb) continue; // no running compute threads or vb processing not completed
            
            zip_output_processed_vb (processed_vb, vcf_file, z_file, false);

            dispatcher_finalize_one_vb (dispatcher, vcf_file, z_file->vcf_data_so_far,
                                        z_file->disk_so_far - z_file->disk_at_beginning_of_this_vcf_file);

        }        
        
        // PRIORITY 3: If there is no variant block available to compute or to output, but input is not exhausted yet - read one
        else if (!next_vb && !dispatcher_is_input_exhausted (dispatcher)) {

            next_vb = dispatcher_generate_next_vb (dispatcher);

            next_vb->first_line = line_i;

            zip_read_variant_block (vcf_file, &line_i, first_data_line, next_vb);
            first_data_line = NULL;

            if (next_vb->num_lines)  // we found some data 
                next_vb->ready_to_dispatch = true;
            else {
                // this vb has no data
                dispatcher_input_exhausted (dispatcher);
                
                dispatcher_finalize_one_vb (dispatcher, vcf_file, z_file->vcf_data_so_far,
                                            z_file->disk_so_far - z_file->disk_at_beginning_of_this_vcf_file);
            }
        }
    } while (!dispatcher_is_done (dispatcher));

    // write terminator section to disk - this will mark the end of this VCF component
    next_vb = dispatcher_generate_next_vb (dispatcher);
    zfile_compress_terminator_section (next_vb);
    zip_output_processed_vb (next_vb, vcf_file, z_file, true);

    // go back and update some fields in the vcf header's section header and genozip header -
    // only if we can go back - i.e. is a normal file, not redirected
    Md5Hash md5;
    if (z_file && z_file->type == GENOZIP && vcf_header_header_pos >= 0) 
        success = zfile_update_vcf_header_section_header (pseudo_vb, vcf_header_header_pos, &md5);

    // if this last file - write the sections, random access and dictionaries
    if (is_last_file) {
        zfile_write_dictionaries (pseudo_vb);
        zfile_write_random_access (pseudo_vb);
        zfile_write_sections (pseudo_vb, DATA_TYPE_VCF, &md5, true);    
    }

finish:
    z_file->disk_size = z_file->disk_so_far;
    z_file->num_lines_single     = 0;
    z_file->vcf_data_size_single = 0;
    memset (&z_file->md5_ctx_single, 0, sizeof (Md5Context));

    vcf_file->vcf_data_size_concat = z_file->vcf_data_size_concat = vcf_file->vcf_data_so_far; // just in case its not set already
    
    dispatcher_finish (&dispatcher, &last_variant_block_i);
}
