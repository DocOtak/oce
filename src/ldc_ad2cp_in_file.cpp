/* vim: set expandtab shiftwidth=2 softtabstop=2 tw=70: */

#include <Rcpp.h>
using namespace Rcpp;

// Cross-reference work:
// 1. update ../src/registerDynamicSymbol.c with an item for this
// 2. main code should use the autogenerated wrapper in ../R/RcppExports.R


// The next items are specific to ad2cp, as is the whole
// format, but I want to define these here to make the
// code clearer. See [1 sec 6.1].
#define SYNC 0xA5
#define HEADER_SIZE 10
#define FAMILY 0x10

// allowed: 0x15-0x18, ox1a-0x1f, 0xa0
// allowed: 21-24, 26-31, 160
#define NID_ALLOWED 11
int ID_ALLOWED[NID_ALLOWED]={21, 22, 23, 24, 26, 27, 28, 29, 30, 31, 160};


/*

   Locate (header+data) for Nortek ad2cp

   @param filename character string indicating the file name.

   @param from integer giving the index of the first ensemble (AKA
   profile) to retrieve. The R notation is used, i.e. from=1 means the
   first profile.

   @param to integer giving the index of the last ensemble to retrieve.
   As a special case, setting this to 0 will retrieve *all* the data
   within the file.

   @param by integer giving increment of the sequence, as for seq(), i.e.
   a value of 1 means to retrieve all the profiles, while a value of 2
   means to get every second profile.

   @value a list containing 'index', 'length' and 'id'. The last of
   these mean: 0x16=21 for Burst Data Record; 0x16=22 for Average Data
   Record; 0x17=23 for Bottom Track Data Record; 0x18=24 for
   Interleaved Burst Data Record (beam 5); 0xA0=160 forString Data
   Record, eg. GPS NMEA data, comment from the FWRITE command.

   @examples

   system("R CMD SHLIB ldc_ad2cp_in_file.c")
   f <- "/Users/kelley/Dropbox/oce_ad2cp/labtestsig3.ad2cp"
   dyn.load("ldc_ad2cp_in_file.so")
   a <- .Call("ldc_ad2cp_in_file", f, 1, 10, 1)

@section: notes

Table 6.1 (header definition):

+-----------------+------------------------+------------------------------------------------------+
| Sync            | 8 bits                 | Always 0xA5                                          |
+-----------------|------------------------|------------------------------------------------------+
| Header Size     | 8 bits (unsigned)      | Size (number of bytes) of the Header.                |
+-----------------|------------------------|------------------------------------------------------+
| ID              | 8 bits                 | Defines type of the following Data Record            |
|                 |                        | 0x16=21  – Burst Data Record.                        |
|                 |                        | 0x16=22  – Average Data Record.                      |
|                 |                        | 0x17=23  – Bottom Track Data Record.                 |
|                 |                        | 0x18=24  – Interleaved Burst Data Record (beam 5).   |
|                 |                        | 0xA0=160 - String Data Record, eg. GPS NMEA data,    |
|                 |                        |            comment from the FWRITE command.          |
+-----------------|------------------------|------------------------------------------------------|
| Family          | 8 bits                 | Defines the Instrument Family. 0x10 – AD2CP Family   |
+-----------------|------------------------|------------------------------------------------------+
| Data Size       | 16 bits (unsigned)     | Size (number of bytes) of the following Data Record. |
|                 | OR 32 bits (unsigned)  |                                                      |
+-----------------|------------------------|------------------------------------------------------+
| Data Checksum   | 16 bits                | Checksum of the following Data Record.               |
+-----------------|------------------------|------------------------------------------------------+
| Header Checksum | 16 bits                | Checksum of all fields of the Header                 |
|                 |                        | (excepts the Header Checksum itself).                |
+-----------------+------------------------+------------------------------------------------------+

Note that the code examples in [1] suggest that the checksums are also unsigned, although
that is not stated in the table. I think the same can be said of [2]. But I may be wrong,
since I am not getting checksums appropriately.

@references

1. "Integrators Guide AD2CP_A.pdf", provided to me privately by
(person 1) in early April of 2017.

2. https://github.com/aodn/imos-toolbox/blob/master/Parser/readAD2CPBinary.m

@author

Dan Kelley

*/

// Check the header checksum.
//
// The code for this differs from that suggested by Nortek,
// because we don't use a specific (msoft) compiler, so we
// do not have access to misaligned_load16(). Also, I don't
// think this will work properly if the number of bytes
// is odd.
//
// FIXME: handle odd-numbered byte case.
unsigned short cs(unsigned char *data, unsigned short size)
{
  // It might be worth checking the matlab code at
  //     https://github.com/aodn/imos-toolbox/blob/master/Parser/readAD2CPBinary.m
  // for context, if problems ever arise.
  unsigned short checksum = 0xB58C;
  for (int i = 0; i < size; i += 2) {
    checksum += (unsigned short)data[i] + 256*(unsigned short)data[i+1];
  }
  return(checksum);
}


// [[Rcpp::export]]
List do_ldc_ad2cp_in_file(CharacterVector filename, IntegerVector from, IntegerVector to, IntegerVector by, IntegerVector DEBUG)
{
  int broken_end = 0;
  int debug = DEBUG[0];
  if (debug < 0)
    debug = 0;
  std::string fn = Rcpp::as<std::string>(filename(0));
  FILE *fp = fopen(fn.c_str(), "rb");
  if (!fp)
    ::Rf_error("cannot open file '%s'\n", fn.c_str());

  if (from[0] < 0)
    ::Rf_error("'from' must be positive but it is %d", from[0]);
  //unsigned int from_value = from[0];
  if (to[0] < 0)
    ::Rf_error("'to' must be positive but it is %d", to[0]);
  unsigned int to_value = to[0];
  if (by[0] < 0)
    ::Rf_error("'by' must be positive but it is %d", by[0]);
  //unsigned int by_value = by[0];
  if (debug)
    Rprintf("do_ldc_ad2cp_in_file(filename='%s', from=%d, to=%d, by=%d)\n",
        fn.c_str(), from[0], to[0], by[0]);

  // FIXME: should we just get this from R? and do we even need it??
  fseek(fp, 0L, SEEK_END);
  unsigned long int fileSize = ftell(fp);
  fseek(fp, 0L, SEEK_SET);
  if (debug)
    Rprintf("  fileSize=%d\n", fileSize);
  unsigned int chunk = 0;
  unsigned int cindex = 0;

  // Ensure that the first byte we point to equals SYNC.
  // In a conventional file, starting with a SYNC char, this
  // just gets a byte and puts it back, leaving cindex=0.
  // But if the file does not start with a SYNC char, e.g.
  // if this is a fragment, we step through the file
  // until we find a SYNC, setting cindex appropriately.
  int c;
  while (1) {
    c = getc(fp);
    if (EOF == c) {
      ::Rf_error("this file does not contain a single 0x", SYNC, " byte");
      break;
    }
    if (SYNC == c) {
      fseek(fp, -1, SEEK_CUR);
      break;
    }
    cindex++;
  }
  // The table in [1 sec 6.1] says header pieces are 10 bytes
  // long, so once we get an 0xA5, we'll try to get 9 more bytes.
  unsigned char bytes2[2]; // for data_size
  unsigned char bytes4[4]; // for data_size
  struct header {
    unsigned char sync;
    unsigned char header_size;
    unsigned char id;
    unsigned char family;
    unsigned long data_size; // may be 2 bytes or 4 bytes in header
    unsigned short data_checksum;
    unsigned short header_checksum;
  } header;
  unsigned int dbuflen = 10000; // may be increased later
  unsigned char *dbuf = (unsigned char *)Calloc((size_t)dbuflen, unsigned char);
  unsigned int nchunk = 100000;
  unsigned int *index_buf = (unsigned int*)Calloc((size_t)nchunk, unsigned int);
  unsigned int *length_buf = (unsigned int*)Calloc((size_t)nchunk, unsigned int);
  unsigned int *id_buf = (unsigned int*)Calloc((size_t)nchunk, unsigned int);
  while (chunk < to_value) {// FIXME: use whole file here
    if (chunk > nchunk - 1) {
      if (debug)
        Rprintf("  about to increase buffer size from %d\n", nchunk);
      nchunk = (unsigned int) floor(chunk * 1.4); // increase buffer size by sqrt(2)
      index_buf = (unsigned int*)Realloc(index_buf, nchunk, unsigned int);
      length_buf = (unsigned int*)Realloc(length_buf, nchunk, unsigned int);
      id_buf = (unsigned int*)Realloc(id_buf, nchunk, unsigned int);
      if (debug)
        Rprintf("  successfully increased buffer size to %d\n", nchunk);
    }
    size_t bytes_read;
    // read/check sync byte
    if (1 != fread(&header.sync, 1, 1, fp))
      ::Rf_error("cannot read header.sync at cindex=%d\n", cindex);
    if (header.sync != SYNC)
      ::Rf_error("expected header.sync to be 0x%02x but it was 0x%02x at byte %d", SYNC, header.sync, cindex);
    if (1 != fread(&header.header_size, 1, 1, fp))
      ::Rf_error("cannot read header_size at cindex=%d\n", cindex);
    if (header.header_size < 2)
      ::Rf_error("impossible header.header_size %d at cindex=%d\n", header.header_size, cindex);
    if (1 != fread(&header.id, 1, 1, fp))
      ::Rf_error("cannot read header.id at cindex=%d\n", cindex);
    if (1 != fread(&header.family, 1, 1, fp))
      ::Rf_error("cannot read header.family at cindex=%d\n", cindex);
    unsigned char header_buffer[2];
    if (header.header_size == 10) {
      if (2 != fread(bytes2, 1, 2, fp))
        ::Rf_error("cannot read bytes2 (for 16 bit header_size) at cindex=%d\n", cindex);
      header.data_size = bytes2[0] + 256 * bytes2[1];
    } else if (header.header_size == 12) {
      if (4 != fread(bytes4, 1, 4, fp))
        ::Rf_error("cannot read bytes4 (for 32 bit header_size) at cindex=%d\n", cindex);
      header.data_size = bytes4[0] +
        256 * (bytes4[1] +
            256 * (bytes4[2] +
              256 * bytes4[3]));
    } else {
      ::Rf_error("header_size must be 10 or 12 at cindex=%d\n", cindex);
    }
    if (2 != fread(&bytes2, 1, 2, fp))
      ::Rf_error("cannot read header.data_checksum at cindex=%d\n", cindex);
    header.data_checksum = bytes2[0] + 256 * bytes2[1];
    if (2 != fread(&bytes2, 1, 2, fp))
      ::Rf_error("cannot read header.header_checksum at cindex=%d\n", cindex);
    header.header_checksum = bytes2[0] + 256 * bytes2[1];
    if (debug && debug > 1)
      Rprintf("  at cindex=%4d chunk=%4d: sync=0x%02x size=%d id=0x%02x family=0x%02x dataSize=%d dataChecksum=%d haderChecksum=%d\n",
          cindex, chunk, header.sync, header.header_size, header.id, header.family, header.data_size,
          header.data_checksum, header.header_checksum);
    cindex = cindex + header.header_size;
    index_buf[chunk] = cindex;
    length_buf[chunk] = header.data_size;

    int found = 0;
    for (int idi = 0; idi < NID_ALLOWED; idi++) {
      if (header.id == ID_ALLOWED[idi]) {
        found = 1;
        break;
      }
    }
    //if (!found)
    //  Rprintf("warning: ldc_ad2cp_in_file() found odd header.id (0x%02x) at chunk %d, cindex=%d\n", header.id, chunk, cindex);
    id_buf[chunk] = header.id;
    // Check the header checksum.
    //FIXME: unsigned short hbufcs = cs(hbuf, header.header_size-2); // FIXME: -2 probably wrong now
    //FIXME: if (hbufcs != header.header_checksum) {
    //FIXME:   Rprintf("warning: at cindex=%d, header checksum is %d but it should be %d\n",
    //FIXME:       cindex, hbufcs, header.header_checksum);
    //FIXME: }
    // Increase size of data buffer, if required.
    if (header.data_size > dbuflen) { // expand the buffer if required
      dbuflen = header.data_size;
      dbuf = (unsigned char *)Realloc(dbuf, dbuflen, unsigned char);
    }
    // Read the data
    bytes_read = fread(dbuf, 1, header.data_size, fp);
    // Check that we got all the data
    if (bytes_read != header.data_size) {
      Rprintf("warning: ldc_ad2cp_in_file() ran out of file on data chunk near cindex=%d; wanted %d bytes but got only %d\n",
          cindex, header.data_size, bytes_read);
      break;
    }
    // Compare data checksum to the value stated in the header
    unsigned short dbufcs;
    dbufcs = cs(dbuf, header.data_size);
    if (dbufcs != header.data_checksum) {
      Rprintf("warning: ldc_ad2cp_in_file() at cindex=%d, data checksum is %d but it should be %d\n",
          cindex, dbufcs, header.data_checksum);
      broken_end = 1;
    }
    cindex += header.data_size;
    chunk++;
  }
  IntegerVector index(chunk), length(chunk), id(chunk);
  for (unsigned int i = 0; i < chunk; i++) {
    index[i] = index_buf[i];
    length[i] = length_buf[i];
    id[i] = id_buf[i];
  }
  Free(index_buf);
  Free(length_buf);
  Free(id_buf);
  if (debug)
    Rprintf("} # do_ldc_ad2cp_in_file\n");
  return(List::create(Named("index")=index,
        Named("length")=length,
        Named("id")=id,
        Named("brokenEnd")=broken_end));
}
