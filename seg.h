// ------------------------------------------------------------------
//   seg.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef SEGREGATE_INCLUDED
#define SEGREGATE_INCLUDED

#include "genozip.h"
#include "sections.h"

typedef const char *SegDataLineFuncType (VBlockP vb, const char *field_start_line, uint32_t vb_line_i);

extern void seg_all_data_lines (VBlockP vb, SegDataLineFuncType seg_data_line, unsigned sizeof_line, 
                                int first_field, int last_field, 
                                const char **field_names, SectionType first_field_dict_section);

extern DictIdType seg_get_format_subfield (const char **data, uint32_t *len, unsigned line_i);

extern const char *seg_get_next_item (const char *str, int *str_len, bool allow_newline, bool allow_tab, bool allow_colon, unsigned vb_line_i, // line in vcf file,
                                      unsigned *len, char *separator, bool *has_13, // out
                                      const char *item_name);

extern void seg_store (VBlockP vb, 
                       bool *dst_is_spillover, uint32_t *dst_start, uint32_t *dst_len, // out
                       BufferP src_buf, uint32_t size, // Either src_buf OR size must be given
                       const char *limit_txt_data, // we cannot store in txt starting here. if NULL always allocates in txt_data_spillover
                       bool align32); // does start address need to be 32bit aligned to prevent aliasing issues

extern uint32_t seg_one_field (VBlockP vb, const char *str, unsigned len, unsigned vb_line_i, int f, 
                               SectionType sec_b250, bool *is_new);

extern int32_t seg_pos_field (VBlockP vb, int32_t last_pos, int pos_field, SectionType sec_pos_b250,
                              const char *pos_str, unsigned pos_len, unsigned vb_line_i);
                               

// ---------
// VCF Stuff
// ---------
//extern const char *seg_vcf_data_line (VBlockP vb_, const char *field_start_line, uint32_t vb_line_i);
extern SegDataLineFuncType seg_vcf_data_line;

extern void seg_vcf_complete_missing_lines (VBlockVCFP vb);

// ---------
// SAM Stuff
// ---------
extern SegDataLineFuncType seg_sam_data_line;

#endif