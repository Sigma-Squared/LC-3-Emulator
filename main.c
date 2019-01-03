#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

#define REG_MASK 0b111

uint16_t memory[UINT16_MAX];

enum {
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};
uint16_t reg[R_COUNT];

enum {
    OP_BR = 0, /* branch */
    OP_ADD,    /* add  */
    OP_LD,     /* load */
    OP_ST,     /* store */
    OP_JSR,    /* jump register */
    OP_AND,    /* bitwise and */
    OP_LDR,    /* load register */
    OP_STR,    /* store register */
    OP_RTI,    /* unused */
    OP_NOT,    /* bitwise not */
    OP_LDI,    /* load indirect */
    OP_STI,    /* store indirect */
    OP_JMP,    /* jump */
    OP_RES,    /* reserved (unused) */
    OP_LEA,    /* load effective address */
    OP_TRAP    /* execute trap */
};

enum {
    FL_POS = 0b001,
    FL_ZRO = 0b010,
    FL_NEG = 0b100
};

enum {
    TRAP_GETC  = 0b100000,  /* get character from keyboard */
    TRAP_OUT   = 0b100001,  /* output a character */
    TRAP_PUTS  = 0b100010,  /* output a word string */
    TRAP_IN    = 0b100011,  /* input a string */
    TRAP_PUTSP = 0b100100,  /* output a byte string */
    TRAP_HALT  = 0b100101   /* halt the program */
};

enum {
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02 /* keyboard data */
};

uint16_t instr; /* current instruction */
int running = 1;

uint16_t check_key() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

uint16_t mem_read(uint16_t addr) {
    if (addr == MR_KBSR) {
        if (check_key()) {
            memory[MR_KBSR] = 1 << 15;
            memory[MR_KBDR] = getchar();
        } else {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[addr];
}

void mem_write(uint16_t addr, uint16_t val) {
    memory[addr] = val;
}

uint16_t sign_extend(uint16_t x, int bit_count) {
    if ((x >> (bit_count-1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

uint16_t lastnbits(int n) {
    uint16_t mask = (1 << n) - 1;
    return sign_extend(instr & mask, n);
}

uint16_t nthbit(int n) {
    return (instr >> n) & 1;
}

uint16_t basereg() {
    return (instr >> 6) & REG_MASK;
}

void update_flags(uint16_t r) {
    if (reg[r] == 0) {
        reg[R_COND] = FL_ZRO;
    } else if (reg[r] >> 15) {
        reg[R_COND] = FL_NEG;
    } else {
        reg[R_COND] = FL_POS;
    }
}

void add_and(int and) {
    uint16_t dr = (instr>>9) & REG_MASK;
    uint16_t sr1 = (instr>>6) & REG_MASK;
    uint16_t imm_flag = nthbit(5);

    uint16_t opr2;
    if (imm_flag) {
        opr2 = sign_extend(instr & 0b11111, 5);
    } else {
        opr2 = reg[instr & REG_MASK];
    }
    if (and) {
        reg[dr] = reg[sr1] & opr2;
    } else {
        reg[dr] = reg[sr1] + opr2;
    }
    update_flags(dr);
}

void br() {
    uint16_t n = nthbit(11);
    uint16_t z = nthbit(10);
    uint16_t p = nthbit(9);
    if ((n && reg[R_COND]==FL_NEG) || (z && reg[R_COND]==FL_ZRO) || (p && reg[R_COND]==FL_POS)) {
        reg[R_PC] += lastnbits(9);
    }
}

void jmp_ret() {
    reg[R_PC] = reg[basereg()];
}

void jsr() {
    reg[R_R7] = reg[R_PC];
    uint16_t imm_flag  = nthbit(11);
    if (imm_flag) {
        reg[R_PC] += lastnbits(11);
    } else {
        reg[R_PC] = reg[basereg()];
    }
}

void ld() {
    uint16_t dr = (instr>>9) & REG_MASK;
    reg[dr] = mem_read(reg[R_PC] + lastnbits(9));
    update_flags(dr);
}

void ldi() {
    uint16_t dr = (instr>>9) & REG_MASK;
    reg[dr] = mem_read(mem_read(reg[R_PC] + lastnbits(9)));
    update_flags(dr);
}

void ldr() {
    uint16_t dr = (instr>>9) & REG_MASK;
    reg[dr] = mem_read(reg[basereg()] + lastnbits(6));
    update_flags(dr);
}

void lea() {
    uint16_t dr = (instr>>9) & REG_MASK;
    reg[dr] = reg[R_PC] + lastnbits(9);
    update_flags(dr);
}

void not() {
    uint16_t dr  = (instr>>9) & REG_MASK;
    uint16_t sr = (instr>>6) & REG_MASK;
    reg[dr] = ~reg[sr];
    update_flags(dr);
}

void st() {
    uint16_t sr = (instr>>9) & REG_MASK;
    mem_write(reg[R_PC] + lastnbits(9), reg[sr]);
}

void sti() {
    uint16_t sr = (instr>>9) & REG_MASK;
    mem_write(mem_read(reg[R_PC] + lastnbits(9)), reg[sr]);
}

void str() {
    uint16_t sr = (instr>>9) & REG_MASK;
    mem_write(reg[basereg()] + lastnbits(6), reg[sr]);
}

void trap_getc() {
    reg[R_R0] = (uint16_t)getchar();
}

void trap_out() {
    putc((char)reg[R_R0], stdout);
    fflush(stdout);
}

void trap_puts() {
    for(uint16_t* p = memory + reg[R_R0]; *p; p++) {
        putc((char) *p, stdout);
    }
    fflush(stdout);
}

void trap_in() {
    printf("Enter a character: ");
    reg[R_R0] = (uint16_t)getchar();
}

void trap_putsp() {
    for(uint16_t* p = memory + reg[R_R0]; *p; p++) {
        char c1 = (*p) & 0xFF;
        putc(c1, stdout);
        char c2 = (*p) >> 8;
        if (c2) putc(c2, stdout);
    }
    fflush(stdout);
}

void trap_halt() {
    running = 0;
}

void trap() {
    switch (instr & 0xFF) {
        case TRAP_GETC:
        trap_getc();
        break;
        case TRAP_OUT:
        trap_out();
        break;
        case TRAP_PUTS:
        trap_puts();
        break;
        case TRAP_IN:
        trap_in();
        break;
        case TRAP_PUTSP:
        trap_putsp();
        break;
        case TRAP_HALT:
        trap_halt();
        break;
    }
}

uint16_t swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}

void read_image_file(FILE* file) {
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = UINT16_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);
    while (read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}

int read_image(const char* fname) {
    FILE* file = fopen(fname, "rb");
    if (!file) return 0;
    read_image_file(file);
    fclose(file);
    return 1;
}

struct termios original_tio;

void disable_input_buffering() {
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering() {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

void handle_interrupt(int signal) {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

int main(int argc, const char** argv) {

    if (argc < 2) {
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }
    
    for (int j=1; j<argc; j++) {
        if (!read_image(argv[j])) {
            printf("Failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    while (running) {
        instr = mem_read(reg[R_PC]++);
        uint16_t opcode = instr >> 12;

        switch (opcode) {
            case OP_ADD:
            add_and(0);
            break;
            case OP_AND:
            add_and(1);
            break;
            case OP_NOT:
            not();
            break;
            case OP_BR:
            br();
            break;
            case OP_JMP:
            jmp_ret();
            break;
            case OP_JSR:
            jsr();
            break;
            case OP_LD:
            ld();
            break;
            case OP_LDI:
            ldi();
            break;
            case OP_LDR:
            ldr();
            break;
            case OP_LEA:
            lea();
            break;
            case OP_ST:
            st();
            break;
            case OP_STI:
            sti();
            break;
            case OP_STR:
            str();
            break;
            case OP_TRAP:
            trap();
            break;
            case OP_RES:
            case OP_RTI:
            default:
            printf("ERROR: Unsupported opcode. Aborted.");
            return 1;
        }
    }

    restore_input_buffering();
    return 0;
}