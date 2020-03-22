// ------------------------------------------------------------------
//   file.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define Z_LARGE64
#ifdef __APPLE__
    #define off64_t __int64_t
#endif
#include <zlib.h>
#include <bzlib.h>
#include "genozip.h"
#include "move_to_front.h"
#include "file.h"
#include "stream.h"

// globals
File *z_file   = NULL;
File *vcf_file = NULL;

// global pointers - so the can be compared eg "if (mode == READ)"
const char *READ  = "rb";  // use binary mode (b) in read and write so Windows doesn't add \r
const char *WRITE = "wb";

char *file_exts[] = FILE_EXTS;

static FileType file_get_type (const char *filename)
{
    for (FileType ft=UNKNOWN_EXT+1; ft <= END_OF_EXTS-1; ft++)
        if (file_has_ext (filename, file_exts[ft])) return ft;

    return UNKNOWN_EXT;
}

File *file_open (const char *filename, FileMode mode, FileType expected_type)
{
    ASSERT0 (filename, "Error: filename is null");

    bool file_exists = (access (filename, F_OK) == 0);

    ASSERT (mode != READ  || file_exists, "%s: cannot open %s for reading: %s", global_cmd, filename, strerror(errno));
    ASSERT (mode != WRITE || !file_exists || flag_force || (expected_type==VCF && flag_test), 
            "%s: output file %s already exists: you may use --force to overwrite it", global_cmd, filename);

    File *file = (File *)calloc (1, sizeof(File) + (mode == READ ? READ_BUFFER_SIZE : 0));

    // copy filename 
    unsigned fn_size = strlen (filename) + 1; // inc. \0
    file->name = malloc (fn_size);
    memcpy (file->name, filename, fn_size);

    file->mode = mode;
    file->type = file_get_type (file->name);

    // sanity check
    if (file->mode == READ  && command == ZIP)   ASSERT (file_is_vcf (file), "%s: input file must have one of the following extensions: " VCF_EXTENSIONS, global_cmd);
    if (file->mode == WRITE && command == ZIP)   ASSERT (file->type == VCF_GENOZIP, "%s: output file must have a " VCF_GENOZIP_ " extension", global_cmd);
    if (file->mode == READ  && command == UNZIP) ASSERT (file->type == VCF_GENOZIP, "%s: input file must have a " VCF_GENOZIP_ " extension", global_cmd); 
    if (file->mode == WRITE && command == UNZIP) ASSERT (file->type == VCF,         "%s: output file must have a .vcf extension", global_cmd); 

    if (expected_type == VCF) {

        switch (file->type) {
        case VCF:
            // don't actually open the file if we're just testing in genounzip
            if (flag_test && mode == WRITE) return file;

            file->file = fopen (file->name, mode); // "rb"/"wb" so libc on Windows doesn't drop/add '\r' between our code and the disk. we will handle the '\r' explicitly.
            break;

        case VCF_GZ:
        case VCF_BGZ:
            file->file = gzopen64 (file->name, mode);    
            break;

        case VCF_BZ2:
            file->file = BZ2_bzopen (file->name, mode);    
            break;

        case VCF_XZ:
            file->file = stream_create (true, false, false, "xz", filename , "--threads=8", "--decompress", 
                                        "--keep", "--stdout", flag_quiet ? "--quiet" : NULL, NULL).from_stream_stdout;
            break;
        case BCF:
        case BCF_GZ:
        case BCF_BGZ:
            file->file = stream_create (true, false, false, "bcftools", "view", "-Ov", "--threads", "8", 
                                        filename, NULL).from_stream_stdout;
            break;
        
        default:
            ABORT ("%s: unrecognized file type: %s", global_cmd, file->name);
        }
    }
    
    else if (expected_type == VCF_GENOZIP) 
        file->file = fopen (file->name, mode);

    else 
        ABORT ("Error: invalid expected_type: %u", expected_type);

    ASSERT (file->file, "%s: cannot open file %s: %s", global_cmd, file->name, strerror(errno)); // errno will be retrieve even the open() was called through zlib and bzlib 

    if (mode == READ) {

        file->disk_size = file_get_size (file->name);

        if (file->type == VCF)
            file->vcf_data_size_single = file->vcf_data_size_concat = file->disk_size; 

        // initialize read buffer indices
        file->last_read = file->next_read = READ_BUFFER_SIZE;
    }

    return file;
}

File *file_fdopen (int fd, FileMode mode, FileType type, bool initialize_mutex)
{
    File *file = (File *)calloc (1, sizeof(File) + (mode == READ ? READ_BUFFER_SIZE : 0));

    file->file = fdopen (fd, mode==READ ? "rb" : "wb");
    ASSERT (file->file, "%s: Failed to file descriptor %u: %s", global_cmd, fd, strerror (errno));

    file->type = type;
    file->last_read = file->next_read = READ_BUFFER_SIZE;

    return file;
}

void file_close (File **file_p, 
                 bool cleanup_memory) // optional - used to destroy buffers in the file is closed NOT near the end of the execution, eg when dealing with splitting concatenated files
{
    File *file = *file_p;
    *file_p = NULL;

    if (file->file) {

        if (file->type == VCF_GZ || file->type == VCF_BGZ) {
            int ret = gzclose_r((gzFile)file->file);
            ASSERTW (!ret, "Warning: failed to close vcf.gz file: %s", file->name ? file->name : "");
        }
        else if (file->type == VCF_BZ2) {
            BZ2_bzclose((BZFILE *)file->file);
        }
        else {
            int ret = fclose((FILE *)file->file);

            if (ret && !errno) { // this is a telltale sign of a memory overflow
                buf_test_overflows(evb); // failing to close for no reason is a sign of memory issues
                // if its not a buffer - maybe its file->file itself
                fprintf (stderr, "Error: fclose() failed without an error, possible file->file pointer is corrupted\n");
            }

            ASSERTW (!ret, "Warning: failed to close file %s: %s", file->name ? file->name : "", strerror(errno)); // vcf or genozip
        } 
    }

    // free resources if we are NOT near the end of the execution. If we are at the end of the execution
    // it is faster to just let the process die
    if (cleanup_memory) {
            
        if (file->type == VCF_GENOZIP) { // reading or writing a .vcf.genozip (no need to worry about STDIN or STDOUT - they are by definition a single file - so cleaned up when process exits)
            for (unsigned i=0; i < file->num_dict_ids; i++)
                mtf_destroy_context (&file->mtf_ctx[i]);

            if (file->dicts_mutex_initialized) 
                pthread_mutex_destroy (&file->dicts_mutex);

            buf_destroy (&file->dict_data);
            buf_destroy (&file->ra_buf);
            buf_destroy (&file->section_list_buf);
            buf_destroy (&file->section_list_dict_buf);
            buf_destroy (&file->v1_next_vcf_header);
        }

        if (file_is_zip_read(file))
            buf_destroy (&file->vcf_unconsumed_data);

        if (file->name) FREE (file->name);
        
        FREE (file);
    }
}

size_t file_write (File *file, const void *data, unsigned len)
{
    size_t bytes_written = fwrite (data, 1, len, (FILE *)file->file);
    ASSERT (bytes_written, "Error: failed to write %u bytes to %s: %s", 
            len, file->name ? file->name : "(stdout)", strerror(errno));

    return bytes_written;
}

void file_remove (const char *filename, bool fail_quietly)
{
    int ret = remove (filename); 
    ASSERTW (!ret || fail_quietly, "Warning: failed to remove %s: %s", filename, strerror (errno));
}

bool file_has_ext (const char *filename, const char *extension)
{
    if (!filename) return false;

    unsigned ext_len = strlen (extension);
    unsigned fn_len  = strlen (filename);
    
    return fn_len > ext_len && !strncmp (&filename[fn_len-ext_len], extension, ext_len);
}

// get basename of a filename - we write our own basename for Visual C and Windows compatability
const char *file_basename (const char *filename, bool remove_exe, const char *default_basename,
                           char *basename /* optional pre-allocated memory */, unsigned basename_size /* basename bytes */)
{
    if (!filename) filename = default_basename;

    unsigned len = strlen (filename);
    if (remove_exe && file_has_ext (filename, ".exe")) len -= 4; // for Windows

    // get start of basename
    const char *start = filename;
    for (int i=len-1; i >= 0; i--)
        if (filename[i]=='/' || filename[i]=='\\') {
            start = &filename[i+1];
            break;
        }

    len = len - (start-filename);

    if (!basename) 
        basename = (char *)malloc (len + 1); // +1 for \0
    else
        len = MIN (len, basename_size-1);

    sprintf (basename, "%.*s", (int)len, start);

    return basename;
}

// returns true if successful. depending on soft_fail, a failure will either emit an error 
// (and exit) or a warning (and return).
bool file_seek (File *file, int64_t offset, 
                int whence, // SEEK_SET, SEEK_CUR or SEEK_END
                bool soft_fail)
{
    // check if we can just move the read buffers rather than seeking
    if (file->mode == READ && file->next_read != file->last_read && whence == SEEK_SET) {
#ifdef __APPLE__
        int64_t move_by = offset - ftello ((FILE *)file->file);
#else
        int64_t move_by = offset - ftello64 ((FILE *)file->file);
#endif

        // case: move is within read buffer already in memory (ftello shows the disk location after read of the last buffer)
        // we just change the buffer pointers rather than discarding the buffer and re-reading
        if (move_by <= 0 && move_by >= -(int64_t)file->last_read) {
            file->next_read = file->last_read + move_by;
            return true;
        }
    }

#ifdef __APPLE__
    int ret = fseeko ((FILE *)file->file, offset, whence);
#else
    int ret = fseeko64 ((FILE *)file->file, offset, whence);
#endif

    if (soft_fail) {
        if (!flag_stdout) {
            ASSERTW (!ret, errno == EINVAL ? "Error while reading file %s: it is too small%s" 
                                        : "Warning: fseeko failed on file %s: %s", 
                    file_printname (file),  errno == EINVAL ? "" : strerror (errno));
        }
    } 
    else {
        ASSERT (!ret, "Error: fseeko failed on file %s: %s", file_printname (file), strerror (errno));
    }

    // reset the read buffers
    if (!ret) file->next_read = file->last_read = READ_BUFFER_SIZE;

    return !ret;
}

uint64_t file_tell (File *file)
{
#ifdef __APPLE__
    return ftello ((FILE *)file->file);
#else
    return ftello64 ((FILE *)file->file);
#endif
}

uint64_t file_get_size (const char *filename)
{
    struct stat64 st;
    
    int ret = stat64(filename, &st);
    ASSERT (!ret, "Error: failed accessing %s: %s", filename, strerror(errno));
    
    return st.st_size;
}

bool file_is_dir (const char *filename)
{
    struct stat64 st;
    
    int ret = stat64(filename, &st);
    ASSERT (!ret, "Error: failed accessing %s: %s", filename, strerror(errno));
    
    return S_ISDIR (st.st_mode);
}

void file_get_file (VariantBlockP vb, const char *filename, Buffer *buf, const char *buf_name, unsigned buf_param,
                    bool add_string_terminator)
{
    uint64_t size = file_get_size (filename);

    buf_alloc (vb, buf, size + add_string_terminator, 1, buf_name, buf_param);

    FILE *file = fopen (filename, "rb");
    ASSERT (file, "Error: cannot open %s: %s", filename, strerror (errno));

    size_t bytes_read = fread (buf->data, 1, size, file);
    ASSERT (bytes_read == (size_t)size, "Error reading file %s: %s", filename, strerror (errno));

    buf->len = size;

    if (add_string_terminator) buf->data[size] = 0;

    fclose (file);
}
