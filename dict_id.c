// ------------------------------------------------------------------
//   dict_id.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "dict_id.h"
#include "vcf_header.h"

// globals externed in dict_id.h and initialized in dict_id_initialize
uint64_t dict_id_vardata_fields[8] = {0,0,0,0,0,0,0,0},
         dict_id_FORMAT_PL=0, dict_id_FORMAT_GL=0, dict_id_FORMAT_GP=0, 
         dict_id_INFO_AC=0, dict_id_INFO_AF=0, dict_id_INFO_AN=0, dict_id_INFO_DP=0, dict_id_INFO_VQSLOD=0; 

void dict_id_initialize(void) 
{   // note: this uint64_t values will be different in big and little endian machines 
    // (it's ok, they never get stored in the file)
    for (VcfFields f=CHROM; f <= FORMAT; f++)
        dict_id_vardata_fields[f] = dict_id_vardata_field (dict_id_make (vcf_field_names[f], strlen (vcf_field_names[f]))).num; 
    
    dict_id_FORMAT_PL   = dict_id_format_subfield (dict_id_make ("PL", 2)).num;
    dict_id_FORMAT_GP   = dict_id_format_subfield (dict_id_make ("GP", 2)).num;
    dict_id_FORMAT_GL   = dict_id_format_subfield (dict_id_make ("GL", 2)).num;
    
    dict_id_INFO_AC     = dict_id_info_subfield   (dict_id_make ("AC", 2)).num;
    dict_id_INFO_AF     = dict_id_info_subfield   (dict_id_make ("AF", 2)).num;
    dict_id_INFO_AN     = dict_id_info_subfield   (dict_id_make ("AN", 2)).num;
    dict_id_INFO_DP     = dict_id_info_subfield   (dict_id_make ("DP", 2)).num;
    dict_id_INFO_VQSLOD = dict_id_info_subfield   (dict_id_make ("VQSLOD", 6)).num;
}
