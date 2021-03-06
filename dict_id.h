// ------------------------------------------------------------------
//   dict_id.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef DICT_ID_INCLUDED
#define DICT_ID_INCLUDED

#ifndef _MSC_VER // Microsoft compiler
#include <inttypes.h>
#else
#include "compatibility/visual_c_stdint.h"
#endif
#include "genozip.h"
#include "data_types.h"

extern DictIdType dict_id_make (const char *str, unsigned str_len);
extern DictIdType dict_id_make_v2to4 (const char *str, unsigned str_len);

static inline DictIdType dict_id_field (DictIdType dict_id) { dict_id.id[0] = dict_id.id[0] & 0x3f; return dict_id; } // set 2 Msb to 00
static inline DictIdType dict_id_type_1(DictIdType dict_id) { dict_id.id[0] = dict_id.id[0] | 0xc0; return dict_id; } // set 2 Msb to 11
static inline DictIdType dict_id_type_2(DictIdType dict_id) { return dict_id; } // no change - keep Msb 01

#define dict_id_is(dict_id, str) (dict_id_make (str, strlen(str)).num == dict_id_printable (dict_id).num)
static inline bool dict_id_is_field (DictIdType dict_id) { return ((dict_id.id[0] >> 6) == 0); } // 2 MSb of first byte determine dictionary type
static inline bool dict_id_is_type_1(DictIdType dict_id) { return ((dict_id.id[0] >> 6) == 3); }
static inline bool dict_id_is_type_2(DictIdType dict_id) { return ((dict_id.id[0] >> 6) == 1); }


static inline DictIdType dict_id_printable(DictIdType dict_id) { dict_id.id[0] = (dict_id.id[0] & 0x7f) | 0x40; return dict_id; } // set 2 Msb to 01
#define DICT_ID_NONE ((DictIdType)(uint64_t)0)

typedef struct { DictIdType alias, dst; } DictIdAlias;
extern const DictIdAlias *dict_id_aliases;
uint32_t dict_id_num_aliases;

extern BufferP dict_id_create_aliases_buf (void);
extern void dict_id_read_aliases (void) ;

extern DictIdType dict_id_show_one_b250, dict_id_show_one_dict; // arguments of --show-b250-one and --show-dict-one (defined in genozip.c)
extern DictIdType dict_id_dump_one_b250;                        // arguments of --dump-b250-one (defined in genozip.c)

extern uint64_t dict_id_fields[MAX_NUM_FIELDS_PER_DATA_TYPE],
                
                dict_id_FORMAT_PL, dict_id_FORMAT_GL, dict_id_FORMAT_GP, dict_id_FORMAT_DP, dict_id_FORMAT_MIN_DP, // some VCF FORMAT subfields
                dict_id_INFO_AC,  dict_id_INFO_AF, dict_id_INFO_AN, dict_id_INFO_DP, dict_id_INFO_VQSLOD, // some VCF INFO subfields
                dict_id_INFO_END, dict_id_WindowsEOL,

                // standard tags, see here: https://samtools.github.io/hts-specs/SAMtags.pdf
                dict_id_OPTION_AM, dict_id_OPTION_AS, dict_id_OPTION_CM, dict_id_OPTION_E2, dict_id_OPTION_LB, dict_id_OPTION_FI, 
                dict_id_OPTION_H0, dict_id_OPTION_H1, dict_id_OPTION_H2, dict_id_OPTION_MQ, dict_id_OPTION_NH, dict_id_OPTION_NM, 
                dict_id_OPTION_OA, dict_id_OPTION_OC, dict_id_OPTION_PG, dict_id_OPTION_PQ, dict_id_OPTION_PU, dict_id_OPTION_RG, 
                dict_id_OPTION_SA, dict_id_OPTION_SM, dict_id_OPTION_TC, dict_id_OPTION_U2, dict_id_OPTION_UQ, dict_id_OPTION_CC, 
                dict_id_OPTION_MC, dict_id_OPTION_MD,
    
                // bwa tags see here: http://bio-bwa.sourceforge.net/bwa.shtml : "SAM ALIGNMENT FORMAT"
                dict_id_OPTION_X0, dict_id_OPTION_X1, dict_id_OPTION_XA, dict_id_OPTION_XN, dict_id_OPTION_XM, dict_id_OPTION_XO,
                dict_id_OPTION_XG, dict_id_OPTION_XS, dict_id_OPTION_XE,

                // 
                dict_id_OPTION_ZM,

                // biobambam tags 
                dict_id_OPTION_mc, dict_id_OPTION_ms,

                // GATK tags
                dict_id_OPTION_BD, dict_id_OPTION_BI,
                
                // our own
                dict_id_OPTION_STRAND, dict_id_OPTION_RNAME, dict_id_OPTION_POS, dict_id_OPTION_CIGAR,  dict_id_OPTION_MAPQ,
                
                // GVF attributes - standard
                dict_id_ATTR_ID, dict_id_ATTR_Variant_seq, dict_id_ATTR_Reference_seq, dict_id_ATTR_Variant_freq,

                // GVF attributes - from GRCh37/38 etc
                dict_id_ATTR_Dbxref, // example: "dbSNP_151:rs1282280967"
                dict_id_ATTR_ancestral_allele,
                dict_id_ATTR_Variant_effect, // example: "Variant_effect=non_coding_transcript_variant 0 ncRNA ENST00000431238,intron_variant 0 primary_transcript ENST00000431238"
                dict_id_ATTR_sift_prediction,
                dict_id_ATTR_polyphen_prediction,
                dict_id_ATTR_variant_peptide,

                dict_id_ENSTid,  // private genozip dict

                dict_id_FASTA_DESC, dict_id_FASTA_SEQ, dict_id_FASTA_COMMENT;

// list of ctx who's local data is compressed via a callback function
#define LOCAL_COMPRESSOR_CALLBACKS {  \
    VCF_LOCAL_COMPRESSOR_CALLBACKS    \
    GFF3_LOCAL_COMPRESSOR_CALLBACKS   \
    SAM_LOCAL_COMPRESSOR_CALLBACKS    \
    FASTQ_LOCAL_COMPRESSOR_CALLBACKS  \
    FASTA_LOCAL_COMPRESSOR_CALLBACKS  \
}

// aliases - these are used only in PIZ, as a way for multiple dict_id's to get access to the same data storage
// on the ZIP side, the data is just placed directly in the primary ctx
#define DICT_ID_ALIASES { \
    VCF_DICT_ID_ALIASES   \
    SAM_DICT_ID_ALIASES   \
    FASTQ_DICT_ID_ALIASES \
    FASTA_DICT_ID_ALIASES \
    GFF3_DICT_ID_ALIASES  \
    ME23_DICT_ID_ALIASES  \
}

extern void dict_id_initialize (DataType data_type);

// template can be 0 - anything OR a type - must 2 MSb of id[0] are used OR a specific dict_id
// candidate is a specific dict_id that we test for its matching of the template
extern bool dict_id_is_match (DictIdType template, DictIdType candidate);

extern const char *dict_id_display_type (DataType dt, DictIdType dict_id);

// print the dict_id - NOT thread safe, for use in execution-termination messages
extern const char *err_dict_id (DictIdType dict_id);

#endif
