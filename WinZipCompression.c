#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

#define LOCAL_FILE_HEADER_SIGNATURE   0x04034b50
#define CENTRAL_DIR_HEADER_SIGNATURE  0x02014b50
#define END_CENTRAL_DIR_SIGNATURE     0x06054b50

// Write a 2-byte little-endian integer
void write_le16(FILE *f, uint16_t v) {
    uint8_t b[2] = { v & 0xff, (v >> 8) & 0xff };
    fwrite(b,1,2,f);
}

// Write a 4-byte little-endian integer
void write_le32(FILE *f, uint32_t v) {
    uint8_t b[4] = { v & 0xff, (v>>8)&0xff, (v>>16)&0xff, (v>>24)&0xff };
    fwrite(b,1,4,f);
}

// Compute DOS date & time from Unix timestamp
void dos_datetime(time_t t, uint16_t *dos_date, uint16_t *dos_time) {
    struct tm *tm = localtime(&t);
    *dos_time = ((tm->tm_hour & 0x1F) << 11) | ((tm->tm_min & 0x3F) << 5) | ((tm->tm_sec/2) & 0x1F);
    *dos_date = (((tm->tm_year - 80) & 0x7F) << 9) | (((tm->tm_mon+1) & 0x0F) << 5) | (tm->tm_mday & 0x1F);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s output.zip file1 [file2 ...]\n", argv[0]);
        return 1;
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror("fopen"); return 1; }

    int fileCount = argc - 2;
    // Allocate space for central directory entries
    struct {
        char *name;
        uint16_t name_len;
        uint16_t mod_time, mod_date;
        uint32_t crc32;
        uint32_t comp_size;
        uint32_t uncomp_size;
        uint32_t local_header_offset;
    } *entries = calloc(fileCount, sizeof(*entries));

    for (int i = 0; i < fileCount; i++) {
        const char *filename = argv[i+2];
        struct stat st;
        if (stat(filename, &st) < 0) { perror("stat"); continue; }
        time_t mtime = st.st_mtime;
        dos_datetime(mtime, &entries[i].mod_date, &entries[i].mod_time);
        entries[i].name_len = strlen(filename);
        entries[i].name = strdup(filename);
        entries[i].local_header_offset = ftell(out);

        // Read file into memory
        FILE *in = fopen(filename, "rb");
        if (!in) { perror("fopen"); continue; }
        uint32_t uSize = st.st_size;
        uint8_t *inbuf = malloc(uSize);
        fread(inbuf,1,uSize,in);
        fclose(in);

        // Calculate CRC32
        entries[i].crc32 = crc32(0L, Z_NULL, 0);
        entries[i].crc32 = crc32(entries[i].crc32, inbuf, uSize);
        entries[i].uncomp_size = uSize;

        // Compress data (raw deflate)
        uLong bound = compressBound(uSize);
        uint8_t *outbuf = malloc(bound);
        z_stream strm = {0};
        deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        strm.next_in = inbuf;
        strm.avail_in = uSize;
        strm.next_out = outbuf;
        strm.avail_out = bound;
        deflate(&strm, Z_FINISH);
        deflateEnd(&strm);
        uint32_t cSize = bound - strm.avail_out;
        entries[i].comp_size = cSize;

        // --- Write Local File Header ---
        write_le32(out, LOCAL_FILE_HEADER_SIGNATURE);
        write_le16(out, 20);                  // version needed
        write_le16(out, 0);                   // flags
        write_le16(out, 8);                   // deflate
        write_le16(out, entries[i].mod_time);
        write_le16(out, entries[i].mod_date);
        write_le32(out, entries[i].crc32);
        write_le32(out, entries[i].comp_size);
        write_le32(out, entries[i].uncomp_size);
        write_le16(out, entries[i].name_len);
        write_le16(out, 0);                   // extra len = 0
        fwrite(entries[i].name,1,entries[i].name_len,out);
        fwrite(outbuf,1,entries[i].comp_size,out);

        free(inbuf);
        free(outbuf);
    }

    // Record start of central directory
    uint32_t cd_start = ftell(out);

    // --- Write Central Directory ---
    for (int i = 0; i < fileCount; i++) {
        write_le32(out, CENTRAL_DIR_HEADER_SIGNATURE);
        write_le16(out, 20);               // version made by
        write_le16(out, 20);               // version needed
        write_le16(out, 0);                // flags
        write_le16(out, 8);                // deflate
        write_le16(out, entries[i].mod_time);
        write_le16(out, entries[i].mod_date);
        write_le32(out, entries[i].crc32);
        write_le32(out, entries[i].comp_size);
        write_le32(out, entries[i].uncomp_size);
        write_le16(out, entries[i].name_len);
        write_le16(out, 0);                // extra len
        write_le16(out, 0);                // comment len
        write_le16(out, 0);                // disk start
        write_le16(out, 0);                // internal attrs
        write_le32(out, 0);                // external attrs
        write_le32(out, entries[i].local_header_offset);
        fwrite(entries[i].name,1,entries[i].name_len,out);
    }

    uint32_t cd_end = ftell(out);
    uint32_t cd_size = cd_end - cd_start;

    // --- End of Central Directory Record ---
    write_le32(out, END_CENTRAL_DIR_SIGNATURE);
    write_le16(out, 0);               // disk number
    write_le16(out, 0);               // cd start disk
    write_le16(out, fileCount);       // entries on this disk
    write_le16(out, fileCount);       // total entries
    write_le32(out, cd_size);
    write_le32(out, cd_start);
    write_le16(out, 0);               // comment length

    fclose(out);
    printf("Created valid ZIP '%s' with %d file(s)\n", argv[1], fileCount);

    // Cleanup
    for (int i = 0; i < fileCount; i++) free(entries[i].name);
    free(entries);
    return 0;
}
