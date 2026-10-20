/* pds.h — SGX Series-5 PDS (Programmable Data Sequencer) instruction decoder.
 *
 * A PDS program is a data segment (constants, in two banks ds0/ds1) followed by
 * 32-bit little-endian instructions. The EDM is entered through such programs
 * (spec §13.8). Field layouts in pds.c were derived black-box, by probing the
 * reference disassembler with single-bit flips and value sweeps and checking
 * the result against random words (pds_oracle_check.py); no vendor source was
 * used. */
#ifndef PDS_H
#define PDS_H
#include <stdint.h>

/* Decode one PDS instruction into text (oracle-compatible syntax). Returns 1 if
 * decoded; 0 for reserved/unknown encodings, with text set to ".word 0x…". */
int pds_decode(uint32_t w, char *text, unsigned len);

#endif /* PDS_H */
