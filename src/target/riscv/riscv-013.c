/*
 * Support for RISC-V, debug version 0.13, which is currently (2/4/17) the
 * latest draft.
 */

#include <assert.h>
#include <stdlib.h>
#include <time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target/target.h"
#include "target/algorithm.h"
#include "target/target_type.h"
#include "log.h"
#include "jtag/jtag.h"
#include "target/register.h"
#include "target/breakpoints.h"
#include "helper/time_support.h"
#include "riscv.h"
#include "rtos/riscv_debug.h"
#include "debug_defines.h"
#include "rtos/rtos.h"
#include "program.h"
#include "asm.h"
#include "batch.h"

#define DMI_DATA1 (DMI_DATA0 + 1)

static void riscv013_on_step_or_resume(struct target *target, bool step);
static void riscv013_step_or_resume_current_hart(struct target *target, bool step);
//static riscv_addr_t riscv013_progbuf_addr(struct target *target);
//static riscv_addr_t riscv013_data_size(struct target *target);
//static riscv_addr_t riscv013_data_addr(struct target *target);
//static int riscv013_debug_buffer_register(struct target *target, riscv_addr_t addr);
static void riscv013_clear_abstract_error(struct target *target);

/* Implementations of the functions in riscv_info_t. */
static riscv_reg_t riscv013_get_register(struct target *target, int hartid, int regid);
static void riscv013_set_register(struct target *target, int hartid, int regid, uint64_t value);
static void riscv013_select_current_hart(struct target *target);
static void riscv013_halt_current_hart(struct target *target);
static void riscv013_resume_current_hart(struct target *target);
static void riscv013_step_current_hart(struct target *target);
static void riscv013_on_halt(struct target *target);
static void riscv013_on_step(struct target *target);
static void riscv013_on_resume(struct target *target);
static bool riscv013_is_halted(struct target *target);
static enum riscv_halt_reason riscv013_halt_reason(struct target *target);
static void riscv013_write_debug_buffer(struct target *target, unsigned index,
		riscv_insn_t d);
static riscv_insn_t riscv013_read_debug_buffer(struct target *target, unsigned
		index);
static int riscv013_execute_debug_buffer(struct target *target);
static void riscv013_fill_dmi_write_u64(struct target *target, char *buf, int a, uint64_t d);
static void riscv013_fill_dmi_read_u64(struct target *target, char *buf, int a);
static int riscv013_dmi_write_u64_bits(struct target *target);
static void riscv013_fill_dmi_nop_u64(struct target *target, char *buf);
static int register_read_direct(struct target *target, uint64_t *value, uint32_t number);

/**
 * Since almost everything can be accomplish by scanning the dbus register, all
 * functions here assume dbus is already selected. The exception are functions
 * called directly by OpenOCD, which can't assume anything about what's
 * currently in IR. They should set IR to dbus explicitly.
 */

#define get_field(reg, mask) (((reg) & (mask)) / ((mask) & ~((mask) << 1)))
#define set_field(reg, mask, val) (((reg) & ~(mask)) | (((val) * ((mask) & ~((mask) << 1))) & (mask)))

#define DIM(x)		(sizeof(x)/sizeof(*x))

#define CSR_DCSR_CAUSE_SWBP		1
#define CSR_DCSR_CAUSE_TRIGGER	2
#define CSR_DCSR_CAUSE_DEBUGINT	3
#define CSR_DCSR_CAUSE_STEP		4
#define CSR_DCSR_CAUSE_HALT		5

#define RISCV013_INFO(r) riscv013_info_t *r = get_info(target)

/*** JTAG registers. ***/

typedef enum {
	DMI_OP_NOP = 0,
	DMI_OP_READ = 1,
	DMI_OP_WRITE = 2
} dmi_op_t;
typedef enum {
	DMI_STATUS_SUCCESS = 0,
	DMI_STATUS_FAILED = 2,
	DMI_STATUS_BUSY = 3
} dmi_status_t;

typedef enum {
	RE_OK,
	RE_FAIL,
	RE_AGAIN
} riscv_error_t;

typedef enum slot {
	SLOT0,
	SLOT1,
	SLOT_LAST,
} slot_t;

/*** Debug Bus registers. ***/

#define CMDERR_NONE				0
#define CMDERR_BUSY				1
#define CMDERR_NOT_SUPPORTED	2
#define CMDERR_EXCEPTION		3
#define CMDERR_HALT_RESUME		4
#define CMDERR_OTHER			7

/*** Info about the core being debugged. ***/

struct trigger {
	uint64_t address;
	uint32_t length;
	uint64_t mask;
	uint64_t value;
	bool read, write, execute;
	int unique_id;
};

struct memory_cache_line {
	uint32_t data;
	bool valid;
	bool dirty;
};

typedef struct {
	/* Number of address bits in the dbus register. */
	unsigned abits;
	/* Number of abstract command data registers. */
	unsigned datacount;
	/* Number of words in the Program Buffer. */
	unsigned progbufsize;
	/* Number of Program Buffer registers. */
	/* Number of words in Debug RAM. */
	uint64_t tselect;
	bool tselect_dirty;
	/* The value that mstatus actually has on the target right now. This is not
	 * the value we present to the user. That one may be stored in the
	 * reg_cache. */
	uint64_t mstatus_actual;

	/* Single buffer that contains all register names, instead of calling
	 * malloc for each register. Needs to be freed when reg_list is freed. */
	char *reg_names;
	/* Single buffer that contains all register values. */
	void *reg_values;

	// Number of run-test/idle cycles the target requests we do after each dbus
	// access.
	unsigned int dtmcontrol_idle;

	// This value is incremented every time a dbus access comes back as "busy".
	// It's used to determine how many run-test/idle cycles to feed the target
	// in between accesses.
	unsigned int dmi_busy_delay;

	// This value is increased every time we tried to execute two commands
	// consecutively, and the second one failed because the previous hadn't
	// completed yet.  It's used to add extra run-test/idle cycles after
	// starting a command, so we don't have to waste time checking for busy to
	// go low.
	unsigned int ac_busy_delay;

	bool need_strict_step;

	// Some memoized values
	int progbuf_addr, data_addr, data_size;

	bool abstract_read_csr_supported;
	bool abstract_write_csr_supported;
	bool abstract_read_fpr_supported;
	bool abstract_write_fpr_supported;

	// When a function returns some error due to a failure indicated by the
	// target in cmderr, the caller can look here to see what that error was.
	// (Compare with errno.)
	unsigned cmderr;
} riscv013_info_t;

static void decode_dmi(char *text, unsigned address, unsigned data)
{
	static const struct {
		unsigned address;
		uint64_t mask;
		const char *name;
	} description[] = {
		{ DMI_DMCONTROL, DMI_DMCONTROL_HALTREQ, "haltreq" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_RESUMEREQ, "resumereq" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_HARTRESET, "hartreset" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_HASEL, "hasel" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_HARTSEL, "hartsel" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_NDMRESET, "ndmreset" },
		{ DMI_DMCONTROL, DMI_DMCONTROL_DMACTIVE, "dmactive" },

		{ DMI_DMSTATUS, DMI_DMSTATUS_IMPEBREAK, "impebreak" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ALLRESUMEACK, "allresumeack" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ANYRESUMEACK, "anyresumeack" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ALLNONEXISTENT, "allnonexistent" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ANYNONEXISTENT, "anynonexistent" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ALLUNAVAIL, "allunavail" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ANYUNAVAIL, "anyunavail" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ALLRUNNING, "allrunning" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ANYRUNNING, "anyrunning" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ALLHALTED, "allhalted" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_ANYHALTED, "anyhalted" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_AUTHENTICATED, "authenticated" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_AUTHBUSY, "authbusy" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_DEVTREEVALID, "devtreevalid" },
		{ DMI_DMSTATUS, DMI_DMSTATUS_VERSION, "version" },

		{ DMI_ABSTRACTCS, DMI_ABSTRACTCS_PROGBUFSIZE, "progbufsize" },
		{ DMI_ABSTRACTCS, DMI_ABSTRACTCS_BUSY, "busy" },
		{ DMI_ABSTRACTCS, DMI_ABSTRACTCS_CMDERR, "cmderr" },
		{ DMI_ABSTRACTCS, DMI_ABSTRACTCS_DATACOUNT, "datacount" },

		{ DMI_COMMAND, DMI_COMMAND_CMDTYPE, "cmdtype" },
	};

	text[0] = 0;
	for (unsigned i = 0; i < DIM(description); i++) {
		if (description[i].address == address) {
			uint64_t mask = description[i].mask;
			unsigned value = get_field(data, mask);
			if (value) {
				if (i > 0)
					*(text++) = ' ';
				if (mask & (mask >> 1)) {
					// If the field is more than 1 bit wide.
					sprintf(text, "%s=%d", description[i].name, value);
				} else {
					strcpy(text, description[i].name);
				}
				text += strlen(text);
			}
		}
	}
}

static void dump_field(const struct scan_field *field)
{
	static const char *op_string[] = {"-", "r", "w", "?"};
	static const char *status_string[] = {"+", "?", "F", "b"};

	if (debug_level < LOG_LVL_DEBUG)
		return;

	uint64_t out = buf_get_u64(field->out_value, 0, field->num_bits);
	unsigned int out_op = get_field(out, DTM_DMI_OP);
	unsigned int out_data = get_field(out, DTM_DMI_DATA);
	unsigned int out_address = out >> DTM_DMI_ADDRESS_OFFSET;

	uint64_t in = buf_get_u64(field->in_value, 0, field->num_bits);
	unsigned int in_op = get_field(in, DTM_DMI_OP);
	unsigned int in_data = get_field(in, DTM_DMI_DATA);
	unsigned int in_address = in >> DTM_DMI_ADDRESS_OFFSET;

	log_printf_lf(LOG_LVL_DEBUG,
			__FILE__, __LINE__, "scan",
			"%db %s %08x @%02x -> %s %08x @%02x",
			field->num_bits,
			op_string[out_op], out_data, out_address,
			status_string[in_op], in_data, in_address);

	char out_text[500];
	char in_text[500];
	decode_dmi(out_text, out_address, out_data);
	decode_dmi(in_text, in_address, in_data);
	if (in_text[0] || out_text[0]) {
		log_printf_lf(LOG_LVL_DEBUG, __FILE__, __LINE__, "scan", "%s -> %s",
				out_text, in_text);
	}
}

static riscv013_info_t *get_info(const struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	return (riscv013_info_t *) info->version_specific;
}

/*** Necessary prototypes. ***/

static int register_get(struct reg *reg);

/*** Utility functions. ***/

bool supports_extension(struct target *target, char letter)
{
	RISCV_INFO(r);
	unsigned num;
	if (letter >= 'a' && letter <= 'z') {
		num = letter - 'a';
	} else if (letter >= 'A' && letter <= 'Z') {
		num = letter - 'A';
	} else {
		return false;
	}
	return r->misa & (1 << num);
}

static void select_dmi(struct target *target)
{
	static uint8_t ir_dmi[1] = {DTM_DMI};
	struct scan_field field = {
		.num_bits = target->tap->ir_length,
		.out_value = ir_dmi,
		.in_value = NULL,
		.check_value = NULL,
		.check_mask = NULL
	};

	jtag_add_ir_scan(target->tap, &field, TAP_IDLE);
}

static uint32_t dtmcontrol_scan(struct target *target, uint32_t out)
{
	struct scan_field field;
	uint8_t in_value[4];
	uint8_t out_value[4];

	buf_set_u32(out_value, 0, 32, out);

	jtag_add_ir_scan(target->tap, &select_dtmcontrol, TAP_IDLE);

	field.num_bits = 32;
	field.out_value = out_value;
	field.in_value = in_value;
	jtag_add_dr_scan(target->tap, 1, &field, TAP_IDLE);

	/* Always return to dmi. */
	select_dmi(target);

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("failed jtag scan: %d", retval);
		return retval;
	}

	uint32_t in = buf_get_u32(field.in_value, 0, 32);
	LOG_DEBUG("DTMCS: 0x%x -> 0x%x", out, in);

	return in;
}

static void increase_dmi_busy_delay(struct target *target)
{
	riscv013_info_t *info = get_info(target);
	info->dmi_busy_delay += info->dmi_busy_delay / 10 + 1;
	LOG_INFO("dtmcontrol_idle=%d, dmi_busy_delay=%d, ac_busy_delay=%d",
			info->dtmcontrol_idle, info->dmi_busy_delay,
			info->ac_busy_delay);

	dtmcontrol_scan(target, DTM_DTMCS_DMIRESET);
}

/**
 * exec: If this is set, assume the scan results in an execution, so more
 * run-test/idle cycles may be required.
 */
static dmi_status_t dmi_scan(struct target *target, uint16_t *address_in,
		uint64_t *data_in, dmi_op_t op, uint16_t address_out, uint64_t data_out,
		bool exec)
{
	riscv013_info_t *info = get_info(target);
	uint8_t in[8] = {0};
	uint8_t out[8];
	struct scan_field field = {
		.num_bits = info->abits + DTM_DMI_OP_LENGTH + DTM_DMI_DATA_LENGTH,
		.out_value = out,
		.in_value = in
	};

	assert(info->abits != 0);

	buf_set_u64(out, DTM_DMI_OP_OFFSET, DTM_DMI_OP_LENGTH, op);
	buf_set_u64(out, DTM_DMI_DATA_OFFSET, DTM_DMI_DATA_LENGTH, data_out);
	buf_set_u64(out, DTM_DMI_ADDRESS_OFFSET, info->abits, address_out);

	/* Assume dbus is already selected. */
	jtag_add_dr_scan(target->tap, 1, &field, TAP_IDLE);

	int idle_count = info->dmi_busy_delay;
	if (exec)
		idle_count += info->ac_busy_delay;

	if (idle_count) {
		jtag_add_runtest(idle_count, TAP_IDLE);
	}

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("dmi_scan failed jtag scan");
		return DMI_STATUS_FAILED;
	}

	if (data_in) {
		*data_in = buf_get_u64(in, DTM_DMI_DATA_OFFSET, DTM_DMI_DATA_LENGTH);
	}

	if (address_in) {
		*address_in = buf_get_u32(in, DTM_DMI_ADDRESS_OFFSET, info->abits);
	}

	dump_field(&field);

	return buf_get_u32(in, DTM_DMI_OP_OFFSET, DTM_DMI_OP_LENGTH);
}

static uint64_t dmi_read(struct target *target, uint16_t address)
{
	select_dmi(target);

	dmi_status_t status;
	uint16_t address_in;

	unsigned i = 0;

	// This first loop ensures that the read request was actually sent
	// to the target. Note that if for some reason this stays busy,
	// it is actually due to the previous dmi_read or dmi_write.
	for (i = 0; i < 256; i++) {
		status = dmi_scan(target, NULL, NULL, DMI_OP_READ, address, 0,
				false);
		if (status == DMI_STATUS_BUSY) {
			increase_dmi_busy_delay(target);
		} else if (status == DMI_STATUS_SUCCESS) {
			break;
		} else {
			LOG_ERROR("failed read from 0x%x, status=%d", address, status);
			break;
		}
	}

	if (status != DMI_STATUS_SUCCESS) {
		LOG_ERROR("Failed read from 0x%x; status=%d", address, status);
		abort();
	}

	// This second loop ensures that we got the read
	// data back. Note that NOP can result in a 'busy' result as well, but
	// that would be noticed on the next DMI access we do.
	uint64_t value;
	for (i = 0; i < 256; i++) {
		status = dmi_scan(target, &address_in, &value, DMI_OP_NOP, address, 0,
				false);
		if (status == DMI_STATUS_BUSY) {
			increase_dmi_busy_delay(target);
		} else if (status == DMI_STATUS_SUCCESS) {
			break;
		} else {
			LOG_ERROR("failed read (NOP) at 0x%x, status=%d", address, status);
			break;
		}
	}

	if (status != DMI_STATUS_SUCCESS) {
		LOG_ERROR("Failed read (NOP) from 0x%x; value=0x%" PRIx64 ", status=%d",
				address, value, status);
		abort();
	}

	return value;
}

static void dmi_write(struct target *target, uint16_t address, uint64_t value)
{
	select_dmi(target);
	dmi_status_t status = DMI_STATUS_BUSY;
	unsigned i = 0;

	// The first loop ensures that we successfully sent the write request.
	for (i = 0; i < 256; i++) {
		status = dmi_scan(target, NULL, NULL, DMI_OP_WRITE, address, value,
				address == DMI_COMMAND);
		if (status == DMI_STATUS_BUSY) {
			increase_dmi_busy_delay(target);
		} else if (status == DMI_STATUS_SUCCESS) {
			break;
		} else {
			LOG_ERROR("failed write to 0x%x, status=%d", address, status);
			break;
		}
	}

	if (status != DMI_STATUS_SUCCESS) {
		LOG_ERROR("Failed write to 0x%x;, status=%d",
				address, status);
		abort();
	}

	// The second loop isn't strictly necessary, but would ensure that
	// the write is complete/ has no non-busy errors before returning from this function.
	for (i = 0; i < 256; i++) {
		status = dmi_scan(target, NULL, NULL, DMI_OP_NOP, address, 0,
				false);
		if (status == DMI_STATUS_BUSY) {
			increase_dmi_busy_delay(target);
		} else if (status == DMI_STATUS_SUCCESS) {
			break;
		} else {
			LOG_ERROR("failed write (NOP) at 0x%x, status=%d", address, status);
			break;
		}
	}
	if (status != DMI_STATUS_SUCCESS) {
		LOG_ERROR("failed to write (NOP) 0x%" PRIx64 " to 0x%x; status=%d", value, address, status);
		abort();
	}
}

static void increase_ac_busy_delay(struct target *target)
{
	riscv013_info_t *info = get_info(target);
	info->ac_busy_delay += info->ac_busy_delay / 10 + 1;
	LOG_INFO("dtmcontrol_idle=%d, dmi_busy_delay=%d, ac_busy_delay=%d",
			info->dtmcontrol_idle, info->dmi_busy_delay,
			info->ac_busy_delay);
}

uint32_t abstract_register_size(unsigned width)
{
	switch (width) {
		case 32:
			return set_field(0, AC_ACCESS_REGISTER_SIZE, 2);
		case 64:
			return set_field(0, AC_ACCESS_REGISTER_SIZE, 3);
			break;
		case 128:
			return set_field(0, AC_ACCESS_REGISTER_SIZE, 4);
			break;
		default:
			LOG_ERROR("Unsupported register width: %d", width);
			return 0;
	}
}

static int wait_for_idle(struct target *target, uint32_t *abstractcs)
{
	RISCV013_INFO(info);
	time_t start = time(NULL);
	while (1) {
		*abstractcs = dmi_read(target, DMI_ABSTRACTCS);

		if (get_field(*abstractcs, DMI_ABSTRACTCS_BUSY) == 0) {
			return ERROR_OK;
		}

		if (time(NULL) - start > riscv_command_timeout_sec) {
			info->cmderr = get_field(*abstractcs, DMI_ABSTRACTCS_CMDERR);
			if (info->cmderr != CMDERR_NONE) {
				const char *errors[8] = {
					"none",
					"busy",
					"not supported",
					"exception",
					"halt/resume",
					"reserved",
					"reserved",
					"other" };

				LOG_ERROR("Abstract command ended in error '%s' (abstractcs=0x%x)",
						errors[info->cmderr], *abstractcs);
			}

			LOG_ERROR("Timed out after %ds waiting for busy to go low (abstractcs=0x%x). "
                                  "Increase the timeout with riscv set_command_timeout_sec.",
                                  riscv_command_timeout_sec,
                                  *abstractcs);
			return ERROR_FAIL;
		}
	}
}

static int execute_abstract_command(struct target *target, uint32_t command)
{
	RISCV013_INFO(info);
	LOG_DEBUG("command=0x%x", command);
	dmi_write(target, DMI_COMMAND, command);

	{
		uint32_t dmstatus = 0;
		wait_for_idle(target, &dmstatus);
	}

	uint32_t cs = dmi_read(target, DMI_ABSTRACTCS);
	info->cmderr = get_field(cs, DMI_ABSTRACTCS_CMDERR);
	if (info->cmderr != 0) {
		LOG_DEBUG("command 0x%x failed; abstractcs=0x%x", command, cs);
		// Clear the error.
		dmi_write(target, DMI_ABSTRACTCS, set_field(0, DMI_ABSTRACTCS_CMDERR,
					info->cmderr));
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static riscv_reg_t read_abstract_arg(struct target *target, unsigned index)
{
	riscv_reg_t value = 0;
	unsigned xlen = riscv_xlen(target);
	unsigned offset = index * xlen / 32;
	switch (xlen) {
		default:
			LOG_ERROR("Unsupported xlen: %d", xlen);
			return ~0;
		case 64:
			value |= ((uint64_t) dmi_read(target, DMI_DATA0 + offset + 1)) << 32;
		case 32:
			value |= dmi_read(target, DMI_DATA0 + offset);
	}
	return value;
}

static int write_abstract_arg(struct target *target, unsigned index,
		riscv_reg_t value)
{
	unsigned xlen = riscv_xlen(target);
	unsigned offset = index * xlen / 32;
	switch (xlen) {
		default:
			LOG_ERROR("Unsupported xlen: %d", xlen);
			return ~0;
		case 64:
			dmi_write(target, DMI_DATA0 + offset + 1, value >> 32);
		case 32:
			dmi_write(target, DMI_DATA0 + offset, value);
	}
	return ERROR_OK;
}

/**
 * @size in bits
 */
static uint32_t access_register_command(uint32_t number, unsigned size,
		uint32_t flags)
{
	uint32_t command = set_field(0, DMI_COMMAND_CMDTYPE, 0);
	switch (size) {
		case 32:
			command = set_field(command, AC_ACCESS_REGISTER_SIZE, 2);
			break;
		case 64:
			command = set_field(command, AC_ACCESS_REGISTER_SIZE, 3);
			break;
		default:
			assert(0);
	}

	if (number <= GDB_REGNO_XPR31) {
		command = set_field(command, AC_ACCESS_REGISTER_REGNO,
				0x1000 + number - GDB_REGNO_XPR0);
	} else if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31) {
		command = set_field(command, AC_ACCESS_REGISTER_REGNO,
				0x1020 + number - GDB_REGNO_FPR0);
	} else if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095) {
		command = set_field(command, AC_ACCESS_REGISTER_REGNO,
				number - GDB_REGNO_CSR0);
	} else {
		assert(0);
	}

	command |= flags;

	return command;
}

static int register_read_abstract(struct target *target, uint64_t *value,
		uint32_t number, unsigned size)
{
	RISCV013_INFO(info);

	if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31 &&
			!info->abstract_read_fpr_supported)
		return ERROR_FAIL;
	if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095 &&
			!info->abstract_read_csr_supported)
		return ERROR_FAIL;

	uint32_t command = access_register_command(number, size,
			AC_ACCESS_REGISTER_TRANSFER);

	int result = execute_abstract_command(target, command);
	if (result != ERROR_OK) {
		if (info->cmderr == CMDERR_NOT_SUPPORTED) {
			if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31) {
				info->abstract_read_fpr_supported = false;
				LOG_INFO("Disabling abstract command reads from FPRs.");
			} else if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095) {
				info->abstract_read_csr_supported = false;
				LOG_INFO("Disabling abstract command reads from CSRs.");
			}
		}
		return result;
	}

	if (value)
		*value = read_abstract_arg(target, 0);

	return ERROR_OK;
}

static int register_write_abstract(struct target *target, uint32_t number,
		uint64_t value, unsigned size)
{
	RISCV013_INFO(info);

	if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31 &&
			!info->abstract_write_fpr_supported)
		return ERROR_FAIL;
	if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095 &&
			!info->abstract_write_csr_supported)
		return ERROR_FAIL;

	uint32_t command = access_register_command(number, size,
			AC_ACCESS_REGISTER_TRANSFER |
			AC_ACCESS_REGISTER_WRITE);

	if (write_abstract_arg(target, 0, value) != ERROR_OK) {
		return ERROR_FAIL;
	}

	int result = execute_abstract_command(target, command);
	if (result != ERROR_OK) {
		if (info->cmderr == CMDERR_NOT_SUPPORTED) {
			if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31) {
				info->abstract_write_fpr_supported = false;
				LOG_INFO("Disabling abstract command writes to FPRs.");
			} else if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095) {
				info->abstract_write_csr_supported = false;
				LOG_INFO("Disabling abstract command writes to CSRs.");
			}
		}
		return result;
	}

	return ERROR_OK;
}

static int register_write_direct(struct target *target, unsigned number,
		uint64_t value)
{
	LOG_DEBUG("[%d] reg[0x%x] <- 0x%" PRIx64, riscv_current_hartid(target),
			number, value);

	int result = register_write_abstract(target, number, value,
			riscv_xlen(target));
	if (result == ERROR_OK)
		return ERROR_OK;

	struct riscv_program program;

	riscv_program_init(&program, target);

	uint64_t s0;
	if (register_read_direct(target, &s0, GDB_REGNO_S0) != ERROR_OK)
		return ERROR_FAIL;

	if (register_write_direct(target, GDB_REGNO_S0, value) != ERROR_OK)
		return ERROR_FAIL;

	if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31) {
		if (supports_extension(target, 'D') && riscv_xlen(target) >= 64) {
			riscv_program_insert(&program, fmv_d_x(number - GDB_REGNO_FPR0, S0));
		} else {
			riscv_program_insert(&program, fmv_s_x(number - GDB_REGNO_FPR0, S0));
		}
	} else if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095) {
		riscv_program_csrw(&program, S0, number);
	} else {
		LOG_ERROR("Unsupported register (enum gdb_regno)(%d)", number);
		abort();
	}

	int exec_out = riscv_program_exec(&program, target);
	if (exec_out != ERROR_OK) {
		riscv013_clear_abstract_error(target);
	}

	// Restore S0.
	if (register_write_direct(target, GDB_REGNO_S0, s0) != ERROR_OK)
		return ERROR_FAIL;

	return exec_out;
}

/** Actually read registers from the target right now. */
static int register_read_direct(struct target *target, uint64_t *value, uint32_t number)
{
	int result = register_read_abstract(target, value, number,
			riscv_xlen(target));

	if (result != ERROR_OK) {
		result = ERROR_OK;

		struct riscv_program program;
		riscv_program_init(&program, target);
		assert(number != GDB_REGNO_S0);

		uint64_t s0;
		if (register_read_direct(target, &s0, GDB_REGNO_S0) != ERROR_OK)
			return ERROR_FAIL;

		// Write program to move data into s0.
		if (number >= GDB_REGNO_FPR0 && number <= GDB_REGNO_FPR31) {
			// TODO: Possibly set F in mstatus.
			// TODO: Fully support D extension on RV32.
			if (supports_extension(target, 'D') && riscv_xlen(target) >= 64) {
				riscv_program_insert(&program, fmv_x_d(S0, number - GDB_REGNO_FPR0));
			} else {
				riscv_program_insert(&program, fmv_x_s(S0, number - GDB_REGNO_FPR0));
			}
		} else if (number >= GDB_REGNO_CSR0 && number <= GDB_REGNO_CSR4095) {
			riscv_program_csrr(&program, S0, number);
		} else {
			LOG_ERROR("Unsupported register (enum gdb_regno)(%d)", number);
			abort();
		}

		// Execute program.
		result = riscv_program_exec(&program, target);
		if (result != ERROR_OK) {
			riscv013_clear_abstract_error(target);
		}

		// Read S0
		if (register_read_direct(target, value, GDB_REGNO_S0) != ERROR_OK)
			return ERROR_FAIL;
		// Restore S0.
		if (register_write_direct(target, GDB_REGNO_S0, s0) != ERROR_OK)
			return ERROR_FAIL;
	}

	if (result == ERROR_OK) {
		LOG_DEBUG("[%d] reg[0x%x] = 0x%" PRIx64, riscv_current_hartid(target),
				number, *value);
	}

	return result;
}

/*** OpenOCD target functions. ***/

static int register_get(struct reg *reg)
{
	struct target *target = (struct target *) reg->arch_info;
	uint64_t value = riscv_get_register(target, reg->number);
	buf_set_u64(reg->value, 0, 64, value);
	return ERROR_OK;
}

static int register_write(struct target *target, unsigned int number,
		uint64_t value)
{
	riscv_set_register(target, number, value);
	return ERROR_OK;
}

static int register_set(struct reg *reg, uint8_t *buf)
{
	struct target *target = (struct target *) reg->arch_info;

	uint64_t value = buf_get_u64(buf, 0, riscv_xlen(target));

	LOG_DEBUG("write 0x%" PRIx64 " to %s", value, reg->name);
	struct reg *r = &target->reg_cache->reg_list[reg->number];
	r->valid = true;
	memcpy(r->value, buf, (r->size + 7) / 8);

	return register_write(target, reg->number, value);
}

static struct reg_arch_type riscv_reg_arch_type = {
	.get = register_get,
	.set = register_set
};

static int init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	LOG_DEBUG("init");
	riscv_info_t *generic_info = (riscv_info_t *) target->arch_info;

	generic_info->get_register = &riscv013_get_register;
	generic_info->set_register = &riscv013_set_register;
	generic_info->select_current_hart = &riscv013_select_current_hart;
	generic_info->is_halted = &riscv013_is_halted;
	generic_info->halt_current_hart = &riscv013_halt_current_hart;
	generic_info->resume_current_hart = &riscv013_resume_current_hart;
	generic_info->step_current_hart = &riscv013_step_current_hart;
	generic_info->on_halt = &riscv013_on_halt;
	generic_info->on_resume = &riscv013_on_resume;
	generic_info->on_step = &riscv013_on_step;
	generic_info->halt_reason = &riscv013_halt_reason;
	generic_info->read_debug_buffer = &riscv013_read_debug_buffer;
	generic_info->write_debug_buffer = &riscv013_write_debug_buffer;
	generic_info->execute_debug_buffer = &riscv013_execute_debug_buffer;
	generic_info->fill_dmi_write_u64 = &riscv013_fill_dmi_write_u64;
	generic_info->fill_dmi_read_u64 = &riscv013_fill_dmi_read_u64;
	generic_info->fill_dmi_nop_u64 = &riscv013_fill_dmi_nop_u64;
	generic_info->dmi_write_u64_bits = &riscv013_dmi_write_u64_bits;
	generic_info->version_specific = calloc(1, sizeof(riscv013_info_t));
	if (!generic_info->version_specific)
		return ERROR_FAIL;
	riscv013_info_t *info = get_info(target);

	info->progbufsize = -1;
	info->progbuf_addr = -1;
	info->data_size = -1;
	info->data_addr = -1;

	info->dmi_busy_delay = 0;
	info->ac_busy_delay = 0;

	// Assume all these abstract commands are supported until we learn
	// otherwise.
	// TODO: The spec allows eg. one CSR to be able to be accessed abstractly
	// while another one isn't. We don't track that this closely here, but in
	// the future we probably should.
	info->abstract_read_csr_supported = true;
	info->abstract_write_csr_supported = true;
	info->abstract_read_fpr_supported = true;
	info->abstract_write_fpr_supported = true;

	target->reg_cache = calloc(1, sizeof(*target->reg_cache));
	target->reg_cache->name = "RISC-V Registers";
	target->reg_cache->num_regs = GDB_REGNO_COUNT;

	target->reg_cache->reg_list = calloc(GDB_REGNO_COUNT, sizeof(struct reg));

	const unsigned int max_reg_name_len = 12;
	info->reg_names = calloc(1, GDB_REGNO_COUNT * max_reg_name_len);
	char *reg_name = info->reg_names;
	info->reg_values = NULL;

	for (unsigned int i = 0; i < GDB_REGNO_COUNT; i++) {
		struct reg *r = &target->reg_cache->reg_list[i];
		r->number = i;
		r->caller_save = true;
		r->dirty = false;
		r->valid = false;
		r->exist = true;
		r->type = &riscv_reg_arch_type;
		r->arch_info = target;
		if (i <= GDB_REGNO_XPR31) {
			sprintf(reg_name, "x%d", i);
		} else if (i == GDB_REGNO_PC) {
			sprintf(reg_name, "pc");
		} else if (i >= GDB_REGNO_FPR0 && i <= GDB_REGNO_FPR31) {
			sprintf(reg_name, "f%d", i - GDB_REGNO_FPR0);
		} else if (i >= GDB_REGNO_CSR0 && i <= GDB_REGNO_CSR4095) {
			sprintf(reg_name, "csr%d", i - GDB_REGNO_CSR0);
		} else if (i == GDB_REGNO_PRIV) {
			sprintf(reg_name, "priv");
		}
		if (reg_name[0]) {
			r->name = reg_name;
		}
		reg_name += strlen(reg_name) + 1;
		assert(reg_name < info->reg_names + GDB_REGNO_COUNT * max_reg_name_len);
	}

	return ERROR_OK;
}

static void deinit_target(struct target *target)
{
	LOG_DEBUG("riscv_deinit_target()");
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	free(info->version_specific);
	info->version_specific = NULL;
}

static int examine(struct target *target)
{
	// Don't need to select dbus, since the first thing we do is read dtmcontrol.

	uint32_t dtmcontrol = dtmcontrol_scan(target, 0);
	LOG_DEBUG("dtmcontrol=0x%x", dtmcontrol);
	LOG_DEBUG("  dmireset=%d", get_field(dtmcontrol, DTM_DTMCS_DMIRESET));
	LOG_DEBUG("  idle=%d", get_field(dtmcontrol, DTM_DTMCS_IDLE));
	LOG_DEBUG("  dmistat=%d", get_field(dtmcontrol, DTM_DTMCS_DMISTAT));
	LOG_DEBUG("  abits=%d", get_field(dtmcontrol, DTM_DTMCS_ABITS));
	LOG_DEBUG("  version=%d", get_field(dtmcontrol, DTM_DTMCS_VERSION));
	if (dtmcontrol == 0) {
		LOG_ERROR("dtmcontrol is 0. Check JTAG connectivity/board power.");
		return ERROR_FAIL;
	}
	if (get_field(dtmcontrol, DTM_DTMCS_VERSION) != 1) {
		LOG_ERROR("Unsupported DTM version %d. (dtmcontrol=0x%x)",
				get_field(dtmcontrol, DTM_DTMCS_VERSION), dtmcontrol);
		return ERROR_FAIL;
	}

	riscv013_info_t *info = get_info(target);
	info->abits = get_field(dtmcontrol, DTM_DTMCS_ABITS);
	info->dtmcontrol_idle = get_field(dtmcontrol, DTM_DTMCS_IDLE);

	uint32_t dmcontrol = dmi_read(target, DMI_DMCONTROL);
	uint32_t dmstatus = dmi_read(target, DMI_DMSTATUS);
	if (get_field(dmstatus, DMI_DMSTATUS_VERSION) != 2) {
		LOG_ERROR("OpenOCD only supports Debug Module version 2, not %d "
				"(dmstatus=0x%x)", get_field(dmstatus, DMI_DMSTATUS_VERSION), dmstatus);
		return ERROR_FAIL;
	}

	// Reset the Debug Module.
	dmi_write(target, DMI_DMCONTROL, 0);
	dmi_write(target, DMI_DMCONTROL, DMI_DMCONTROL_DMACTIVE);
	dmcontrol = dmi_read(target, DMI_DMCONTROL);

	LOG_DEBUG("dmcontrol: 0x%08x", dmcontrol);
	LOG_DEBUG("dmstatus:  0x%08x", dmstatus);

	if (!get_field(dmcontrol, DMI_DMCONTROL_DMACTIVE)) {
		LOG_ERROR("Debug Module did not become active. dmcontrol=0x%x",
				dmcontrol);
		return ERROR_FAIL;
	}

	if (!get_field(dmstatus, DMI_DMSTATUS_AUTHENTICATED)) {
		LOG_ERROR("Authentication required by RISC-V core but not "
				"supported by OpenOCD. dmcontrol=0x%x", dmcontrol);
		return ERROR_FAIL;
	}

	if (get_field(dmstatus, DMI_DMSTATUS_ANYUNAVAIL)) {
		LOG_ERROR("The hart is unavailable.");
		return ERROR_FAIL;
	}

	if (get_field(dmstatus, DMI_DMSTATUS_ANYNONEXISTENT)) {
		LOG_ERROR("The hart doesn't exist.");
		return ERROR_FAIL;
	}

	// Check that abstract data registers are accessible.
	uint32_t abstractcs = dmi_read(target, DMI_ABSTRACTCS);
	info->datacount = get_field(abstractcs, DMI_ABSTRACTCS_DATACOUNT);
	info->progbufsize = get_field(abstractcs, DMI_ABSTRACTCS_PROGBUFSIZE);

	/* Before doing anything else we must first enumerate the harts. */
	RISCV_INFO(r);
	r->impebreak = get_field(dmstatus, DMI_DMSTATUS_IMPEBREAK);

	int original_coreid = target->coreid;
	for (int i = 0; i < RISCV_MAX_HARTS; ++i) {
		/* Fake being a non-RTOS targeted to this core so we can see if
		 * it exists.  This avoids the assertion in
		 * riscv_set_current_hartid() that ensures non-RTOS targets
		 * don't touch the harts they're not assigned to.  */
		target->coreid = i;
		r->hart_count = i + 1;
		riscv_set_current_hartid(target, i);

		uint32_t s = dmi_read(target, DMI_DMSTATUS);
		if (get_field(s, DMI_DMSTATUS_ANYNONEXISTENT)) {
			r->hart_count--;
			break;
		}
	}
	target->coreid = original_coreid;

	LOG_DEBUG("Enumerated %d harts", r->hart_count);

	/* Halt every hart so we can probe them. */
	riscv_halt_all_harts(target);

	/* Find the address of the program buffer, which must be done without
	 * knowing anything about the target. */
	for (int i = 0; i < riscv_count_harts(target); ++i) {
		if (!riscv_hart_enabled(target, i))
			continue;

		riscv_set_current_hartid(target, i);

		/* Without knowing anything else we can at least mess with the
		 * program buffer. */
		r->debug_buffer_size[i] = info->progbufsize;

		int result = register_read_abstract(target, NULL, GDB_REGNO_S0, 64);
		if (result == ERROR_OK) {
			r->xlen[i] = 64;
		} else {
			r->xlen[i] = 32;
		}

		r->misa = riscv_get_register_on_hart(target, i, GDB_REGNO_MISA);

		/* Display this as early as possible to help people who are using
		 * really slow simulators. */
		LOG_DEBUG(" hart %d: XLEN=%d, misa=0x%" PRIx64, i, r->xlen[i],
				r->misa);
	}

	/* Then we check the number of triggers availiable to each hart. */
	riscv_enumerate_triggers(target);

	/* Resumes all the harts, so the debugger can later pause them. */
	riscv_resume_all_harts(target);
	target->state = TARGET_RUNNING;
	target_set_examined(target);

	if (target->rtos) {
		riscv_update_threads(target->rtos);
	}

	// Some regression suites rely on seeing 'Examined RISC-V core' to know
	// when they can connect with gdb/telnet.
	// We will need to update those suites if we want to change that text.
	LOG_INFO("Examined RISC-V core; found %d harts",
			riscv_count_harts(target));
	for (int i = 0; i < riscv_count_harts(target); ++i) {
		if (riscv_hart_enabled(target, i)) {
			LOG_INFO(" hart %d: XLEN=%d, %d triggers", i, r->xlen[i],
					r->trigger_count[i]);
		} else {
			LOG_INFO(" hart %d: currently disabled", i);
		}
	}
	return ERROR_OK;
}

static int assert_reset(struct target *target)
{
	RISCV_INFO(r);

	select_dmi(target);

	uint32_t control_base = set_field(0, DMI_DMCONTROL_DMACTIVE, 1);

	if (target->rtos) {
		// There's only one target, and OpenOCD thinks each hart is a thread.
		// We must reset them all.

		// TODO: Try to use hasel in dmcontrol

		// Set haltreq/resumereq for each hart.
		uint32_t control = control_base;
		for (int i = 0; i < riscv_count_harts(target); ++i) {
			if (!riscv_hart_enabled(target, i))
				continue;

			control = set_field(control_base, DMI_DMCONTROL_HARTSEL, i);
			control = set_field(control, DMI_DMCONTROL_HALTREQ,
					target->reset_halt ? 1 : 0);
			dmi_write(target, DMI_DMCONTROL, control);
		}
		// Assert ndmreset
		control = set_field(control, DMI_DMCONTROL_NDMRESET, 1);
		dmi_write(target, DMI_DMCONTROL, control);

	} else {
		// Reset just this hart.
		uint32_t control = set_field(control_base, DMI_DMCONTROL_HARTSEL,
				r->current_hartid);
		control = set_field(control, DMI_DMCONTROL_HALTREQ,
				target->reset_halt ? 1 : 0);
		control = set_field(control, DMI_DMCONTROL_HARTRESET, 1);
		dmi_write(target, DMI_DMCONTROL, control);

		// Read back to check if hartreset is supported.
		uint32_t rb = dmi_read(target, DMI_DMCONTROL);
		if (!get_field(rb, DMI_DMCONTROL_HARTRESET)) {
			// Use ndmreset instead. That will reset the entire device, but
			// that's probably what OpenOCD wants anyway.
			control = set_field(control, DMI_DMCONTROL_HARTRESET, 0);
			control = set_field(control, DMI_DMCONTROL_NDMRESET, 1);
			dmi_write(target, DMI_DMCONTROL, control);
		}
	}

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int deassert_reset(struct target *target)
{
	RISCV_INFO(r);
	RISCV013_INFO(info);
	select_dmi(target);

	LOG_DEBUG("%d", r->current_hartid);

	// Clear the reset, but make sure haltreq is still set
	uint32_t control = 0;
	control = set_field(control, DMI_DMCONTROL_HALTREQ, target->reset_halt ? 1 : 0);
	control = set_field(control, DMI_DMCONTROL_HARTSEL, r->current_hartid);
	control = set_field(control, DMI_DMCONTROL_DMACTIVE, 1);
	dmi_write(target, DMI_DMCONTROL, control);

	uint32_t dmstatus;
	int dmi_busy_delay = info->dmi_busy_delay;
	time_t start = time(NULL);

	if (target->reset_halt) {
		LOG_DEBUG("Waiting for hart to be halted.");
		do {
			dmstatus = dmi_read(target, DMI_DMSTATUS);
			if (time(NULL) - start > riscv_reset_timeout_sec) {
				LOG_ERROR("Hart didn't halt coming out of reset in %ds; "
						"dmstatus=0x%x; "
						"Increase the timeout with riscv set_reset_timeout_sec.",
						riscv_reset_timeout_sec, dmstatus);
				return ERROR_FAIL;
			}
			target->state = TARGET_HALTED;
		} while (get_field(dmstatus, DMI_DMSTATUS_ALLHALTED) == 0);

		control = set_field(control, DMI_DMCONTROL_HALTREQ, 0);
		dmi_write(target, DMI_DMCONTROL, control);

	} else {
		LOG_DEBUG("Waiting for hart to be running.");
		do {
			dmstatus = dmi_read(target, DMI_DMSTATUS);
			if (get_field(dmstatus, DMI_DMSTATUS_ANYHALTED) ||
					get_field(dmstatus, DMI_DMSTATUS_ANYUNAVAIL)) {
				LOG_ERROR("Unexpected hart status during reset. dmstatus=0x%x",
						dmstatus);
				return ERROR_FAIL;
			}
			if (time(NULL) - start > riscv_reset_timeout_sec) {
				LOG_ERROR("Hart didn't run coming out of reset in %ds; "
						"dmstatus=0x%x; "
						"Increase the timeout with riscv set_reset_timeout_sec.",
						riscv_reset_timeout_sec, dmstatus);
				return ERROR_FAIL;
			}
		} while (get_field(dmstatus, DMI_DMSTATUS_ALLRUNNING) == 0);
		target->state = TARGET_RUNNING;
	}
	info->dmi_busy_delay = dmi_busy_delay;
	return ERROR_OK;
}

/**
 * @size in bytes
 */
static void write_to_buf(uint8_t *buffer, uint64_t value, unsigned size)
{
	switch (size) {
		case 8:
			buffer[7] = value >> 56;
			buffer[6] = value >> 48;
			buffer[5] = value >> 40;
			buffer[4] = value >> 32;
		case 4:
			buffer[3] = value >> 24;
			buffer[2] = value >> 16;
		case 2:
			buffer[1] = value >> 8;
		case 1:
			buffer[0] = value;
			break;
		default:
			assert(false);
	}
}

/**
 * Read the requested memory, taking care to execute every read exactly once,
 * even if cmderr=busy is encountered.
 */
static int read_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	RISCV013_INFO(info);

	LOG_DEBUG("reading %d words of %d bytes from 0x%" TARGET_PRIxADDR, count,
			size, address);

	select_dmi(target);

	/* s0 holds the next address to write to
	 * s1 holds the next data value to write
	 */
	uint64_t s0, s1;
	if (register_read_direct(target, &s0, GDB_REGNO_S0) != ERROR_OK)
		return ERROR_FAIL;
	if (register_read_direct(target, &s1, GDB_REGNO_S1) != ERROR_OK)
		return ERROR_FAIL;

	// Write the program (load, increment)
	struct riscv_program program;
	riscv_program_init(&program, target);

	// TODO: riscv_program_fence(&program);
	switch (size) {
		case 1:
			riscv_program_lbr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		case 2:
			riscv_program_lhr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		case 4:
			riscv_program_lwr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		default:
			LOG_ERROR("Unsupported size: %d", size);
			return ERROR_FAIL;
	}
	riscv_program_addi(&program, GDB_REGNO_S0, GDB_REGNO_S0, size);

	if (riscv_program_ebreak(&program) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_write(&program);

	// Write address to S0, and execute buffer.
	if (register_write_direct(target, GDB_REGNO_S0, address) != ERROR_OK)
		return ERROR_FAIL;
	if (execute_abstract_command(target,
				access_register_command(GDB_REGNO_S1, riscv_xlen(target),
				AC_ACCESS_REGISTER_TRANSFER |
				AC_ACCESS_REGISTER_POSTEXEC)) != ERROR_OK)
		return ERROR_FAIL;
	// First read has just triggered. Result is in s1.

	dmi_write(target, DMI_ABSTRACTAUTO,
			1 << DMI_ABSTRACTAUTO_AUTOEXECDATA_OFFSET);

	/* Copying memory might fail because we're going too quickly, in which
	 * case we need to back off a bit and try again. */
	riscv_addr_t cur_addr = address;
	riscv_addr_t fin_addr = address + (count * size);
	LOG_DEBUG("reading until final address 0x%" PRIx64, fin_addr);
	while (cur_addr < fin_addr - size) {
		// Invariant:
		// s0 contains the next address to read
		// s1 contains the data read at the previous address
		// dmdata0 contains the data read at the previous previous address

		unsigned start = (cur_addr - address) / size;
		LOG_DEBUG("creating burst to read address 0x%" TARGET_PRIxADDR
				" up to 0x%" TARGET_PRIxADDR "; start=0x%d", cur_addr, fin_addr, start);
		assert(cur_addr >= address && cur_addr < fin_addr);
		struct riscv_batch *batch = riscv_batch_alloc(target, 32,
				info->dmi_busy_delay + info->ac_busy_delay);

		size_t reads = 0;
		for (riscv_addr_t addr = cur_addr; addr < fin_addr - size; addr += size) {
			riscv_batch_add_dmi_read(batch, DMI_DATA0);

			reads++;
			if (riscv_batch_full(batch))
				break;
		}

		riscv_batch_run(batch);

		// Wait for the target to finish performing the last abstract command,
		// and update our copy of cmderr.
		uint32_t abstractcs = dmi_read(target, DMI_ABSTRACTCS);
		while (get_field(abstractcs, DMI_ABSTRACTCS_BUSY))
			abstractcs = dmi_read(target, DMI_ABSTRACTCS);
		info->cmderr = get_field(abstractcs, DMI_ABSTRACTCS_CMDERR);

		riscv_addr_t next_addr;
		switch (info->cmderr) {
			case CMDERR_NONE:
				LOG_DEBUG("successful (partial?) memory read");
				next_addr = cur_addr + reads * size;
				break;
			case CMDERR_BUSY:
				LOG_DEBUG("memory read resulted in busy response");
				increase_ac_busy_delay(target);
				riscv013_clear_abstract_error(target);

				dmi_write(target, DMI_ABSTRACTAUTO, 0);
				if (register_read_direct(target, &next_addr, GDB_REGNO_S0) != ERROR_OK)
					return ERROR_FAIL;
				dmi_write(target, DMI_ABSTRACTAUTO,
						1 << DMI_ABSTRACTAUTO_AUTOEXECDATA_OFFSET);
				break;
			default:
				LOG_ERROR("error when reading memory, abstractcs=0x%08lx", (long)abstractcs);
				dmi_write(target, DMI_ABSTRACTAUTO, 0);
				riscv_set_register(target, GDB_REGNO_S0, s0);
				riscv_set_register(target, GDB_REGNO_S1, s1);
				riscv013_clear_abstract_error(target);
				riscv_batch_free(batch);
				return ERROR_FAIL;
		}

		// Now read whatever we got out of the batch.
		unsigned rereads = 0;
		for (riscv_addr_t addr = cur_addr - size; addr < next_addr - size; addr += size) {
			if (addr >= address) {
				riscv_addr_t offset = addr - address;
				uint64_t dmi_out = riscv_batch_get_dmi_read(batch, rereads);
				uint32_t value = get_field(dmi_out, DTM_DMI_DATA);
				write_to_buf(buffer + offset, value, size);
				LOG_DEBUG("M[0x%" TARGET_PRIxADDR "] reads 0x%08x", addr, value);
			}

			rereads++;
		}
		riscv_batch_free(batch);

		cur_addr = next_addr;
	}

	dmi_write(target, DMI_ABSTRACTAUTO, 0);

	if (count > 1) {
		// Read the penultimate word.
		uint64_t value = dmi_read(target, DMI_DATA0);
		write_to_buf(buffer + cur_addr - size - address, value, size);
		LOG_DEBUG("M[0x%" TARGET_PRIxADDR "] reads 0x%" PRIx64, cur_addr -
				size, value);
	}

	// Read the last word.
	uint64_t value;
	if (register_read_direct(target, &value, GDB_REGNO_S1) != ERROR_OK)
		return ERROR_FAIL;
	write_to_buf(buffer + cur_addr - address, value, size);
	LOG_DEBUG("M[0x%" TARGET_PRIxADDR "] reads 0x%" PRIx64, cur_addr, value);

	riscv_set_register(target, GDB_REGNO_S0, s0);
	riscv_set_register(target, GDB_REGNO_S1, s1);
	return ERROR_OK;
}

static int write_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	RISCV013_INFO(info);

	LOG_DEBUG("writing %d words of %d bytes to 0x%08lx", count, size, (long)address);

	select_dmi(target);

	/* s0 holds the next address to write to
	 * s1 holds the next data value to write
	 */

	uint64_t s0, s1;
	if (register_read_direct(target, &s0, GDB_REGNO_S0) != ERROR_OK)
		return ERROR_FAIL;
	if (register_read_direct(target, &s1, GDB_REGNO_S1) != ERROR_OK)
		return ERROR_FAIL;

	// Write the program (store, increment)
	struct riscv_program program;
	riscv_program_init(&program, target);

	switch (size) {
		case 1:
			riscv_program_sbr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		case 2:
			riscv_program_shr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		case 4:
			riscv_program_swr(&program, GDB_REGNO_S1, GDB_REGNO_S0, 0);
			break;
		default:
			LOG_ERROR("Unsupported size: %d", size);
			return ERROR_FAIL;
	}

	riscv_program_addi(&program, GDB_REGNO_S0, GDB_REGNO_S0, size);

	if (riscv_program_ebreak(&program) != ERROR_OK)
		return ERROR_FAIL;
	riscv_program_write(&program);

	riscv_addr_t cur_addr = address;
	riscv_addr_t fin_addr = address + (count * size);
	bool setup_needed = true;
	LOG_DEBUG("writing until final address 0x%016" PRIx64, fin_addr);
	while (cur_addr < fin_addr) {
		LOG_DEBUG("transferring burst starting at address 0x%016" PRIx64,
				cur_addr);

		struct riscv_batch *batch = riscv_batch_alloc(
				target,
				32,
				info->dmi_busy_delay + info->ac_busy_delay);

		/* To write another word, we put it in S1 and execute the program. */
		unsigned start = (cur_addr - address) / size;
		for (unsigned i = start; i < count; ++i) {
			unsigned offset = size*i;
			const uint8_t *t_buffer = buffer + offset;

			uint32_t value;
			switch (size) {
				case 1:
					value = t_buffer[0];
					break;
				case 2:
					value = t_buffer[0]
						| ((uint32_t) t_buffer[1] << 8);
					break;
				case 4:
					value = t_buffer[0]
						| ((uint32_t) t_buffer[1] << 8)
						| ((uint32_t) t_buffer[2] << 16)
						| ((uint32_t) t_buffer[3] << 24);
					break;
				default:
					LOG_ERROR("unsupported access size: %d", size);
					return ERROR_FAIL;
			}

			LOG_DEBUG("M[0x%08" PRIx64 "] writes 0x%08x", address + offset, value);
			cur_addr += size;

			if (setup_needed) {
				if (register_write_direct(target, GDB_REGNO_S0,
							address + offset) != ERROR_OK)
					return ERROR_FAIL;

				// Write value.
				dmi_write(target, DMI_DATA0, value);

				// Write and execute command that moves value into S1 and
				// executes program buffer.
				uint32_t command = access_register_command(GDB_REGNO_S1, 32, 
						AC_ACCESS_REGISTER_POSTEXEC |
						AC_ACCESS_REGISTER_TRANSFER |
						AC_ACCESS_REGISTER_WRITE);
				int result = execute_abstract_command(target, command);
				if (result != ERROR_OK)
					return result;

				// Turn on autoexec
				dmi_write(target, DMI_ABSTRACTAUTO,
						1 << DMI_ABSTRACTAUTO_AUTOEXECDATA_OFFSET);

				setup_needed = false;
			} else {
				riscv_batch_add_dmi_write(batch, DMI_DATA0, value);
				if (riscv_batch_full(batch))
					break;
			}
		}

		riscv_batch_run(batch);
		riscv_batch_free(batch);

		// Note that if the scan resulted in a Busy DMI response, it
		// is this read to abstractcs that will cause the dmi_busy_delay
		// to be incremented if necessary.

		uint32_t abstractcs = dmi_read(target, DMI_ABSTRACTCS);
		while (get_field(abstractcs, DMI_ABSTRACTCS_BUSY))
			abstractcs = dmi_read(target, DMI_ABSTRACTCS);
		info->cmderr = get_field(abstractcs, DMI_ABSTRACTCS_CMDERR);
		switch (info->cmderr) {
			case CMDERR_NONE:
				LOG_DEBUG("successful (partial?) memory write");
				break;
			case CMDERR_BUSY:
				LOG_DEBUG("memory write resulted in busy response");
				riscv013_clear_abstract_error(target);
				increase_ac_busy_delay(target);

				dmi_write(target, DMI_ABSTRACTAUTO, 0);
				if (register_read_direct(target, &cur_addr, GDB_REGNO_S0) != ERROR_OK)
					return ERROR_FAIL;
				setup_needed = true;
				break;

			default:
				LOG_ERROR("error when writing memory, abstractcs=0x%08lx", (long)abstractcs);
				dmi_write(target, DMI_ABSTRACTAUTO, 0);
				riscv013_clear_abstract_error(target);
				riscv_set_register(target, GDB_REGNO_S0, s0);
				riscv_set_register(target, GDB_REGNO_S1, s1);
				return ERROR_FAIL;
		}
	}

	dmi_write(target, DMI_ABSTRACTAUTO, 0);

	if (register_write_direct(target, GDB_REGNO_S1, s1) != ERROR_OK)
		return ERROR_FAIL;
	if (register_write_direct(target, GDB_REGNO_S0, s0) != ERROR_OK)
		return ERROR_FAIL;

	return ERROR_OK;
}

static int arch_state(struct target *target)
{
	return ERROR_OK;
}

struct target_type riscv013_target =
{
	.name = "riscv",

	.init_target = init_target,
	.deinit_target = deinit_target,
	.examine = examine,

	.poll = &riscv_openocd_poll,
	.halt = &riscv_openocd_halt,
	.resume = &riscv_openocd_resume,
	.step = &riscv_openocd_step,

	.assert_reset = assert_reset,
	.deassert_reset = deassert_reset,

	.read_memory = read_memory,
	.write_memory = write_memory,

	.arch_state = arch_state,
};

/*** 0.13-specific implementations of various RISC-V helper functions. ***/
static riscv_reg_t riscv013_get_register(struct target *target, int hid, int rid)
{
	LOG_DEBUG("reading register %s on hart %d", gdb_regno_name(rid), hid);

	riscv_set_current_hartid(target, hid);

	uint64_t out;
	riscv013_info_t *info = get_info(target);

	if (rid <= GDB_REGNO_XPR31) {
		register_read_direct(target, &out, rid);
	} else if (rid == GDB_REGNO_PC) {
		register_read_direct(target, &out, GDB_REGNO_DPC);
		LOG_DEBUG("read PC from DPC: 0x%016" PRIx64, out);
	} else if (rid == GDB_REGNO_PRIV) {
		uint64_t dcsr;
		register_read_direct(target, &dcsr, GDB_REGNO_DCSR);
		buf_set_u64((unsigned char *)&out, 0, 8, get_field(dcsr, CSR_DCSR_PRV));
	} else {
		int result = register_read_direct(target, &out, rid);
		if (result != ERROR_OK) {
			LOG_ERROR("Unable to read register %d", rid);
			out = -1;
		}

		if (rid == GDB_REGNO_MSTATUS)
			info->mstatus_actual = out;
	}

	return out;
}

static void riscv013_set_register(struct target *target, int hid, int rid, uint64_t value)
{
	LOG_DEBUG("writing 0x%" PRIx64 " to register %s on hart %d", value,
			gdb_regno_name(rid), hid);

	riscv_set_current_hartid(target, hid);

	if (rid <= GDB_REGNO_XPR31) {
		register_write_direct(target, rid, value);
	} else if (rid == GDB_REGNO_PC) {
		LOG_DEBUG("writing PC to DPC: 0x%016" PRIx64, value);
		register_write_direct(target, GDB_REGNO_DPC, value);
		uint64_t actual_value;
		register_read_direct(target, &actual_value, GDB_REGNO_DPC);
		LOG_DEBUG("  actual DPC written: 0x%016" PRIx64, actual_value);
		assert(value == actual_value);
	} else if (rid == GDB_REGNO_PRIV) {
		uint64_t dcsr;
		register_read_direct(target, &dcsr, GDB_REGNO_DCSR);
		dcsr = set_field(dcsr, CSR_DCSR_PRV, value);
		register_write_direct(target, GDB_REGNO_DCSR, dcsr);
	} else {
		register_write_direct(target, rid, value);
	}
}

static void riscv013_select_current_hart(struct target *target)
{
	RISCV_INFO(r);

	uint64_t dmcontrol = dmi_read(target, DMI_DMCONTROL);
	dmcontrol = set_field(dmcontrol, DMI_DMCONTROL_HARTSEL, r->current_hartid);
	dmi_write(target, DMI_DMCONTROL, dmcontrol);
}

static void riscv013_halt_current_hart(struct target *target)
{
	RISCV_INFO(r);
	LOG_DEBUG("halting hart %d", r->current_hartid);
	assert(!riscv_is_halted(target));

	/* Issue the halt command, and then wait for the current hart to halt. */
	uint32_t dmcontrol = dmi_read(target, DMI_DMCONTROL);
	dmcontrol = set_field(dmcontrol, DMI_DMCONTROL_HALTREQ, 1);
	dmi_write(target, DMI_DMCONTROL, dmcontrol);
	for (size_t i = 0; i < 256; ++i)
		if (riscv_is_halted(target))
			break;

	if (!riscv_is_halted(target)) {
		uint32_t dmstatus = dmi_read(target, DMI_DMSTATUS);
		dmcontrol = dmi_read(target, DMI_DMCONTROL);

		LOG_ERROR("unable to halt hart %d", r->current_hartid);
		LOG_ERROR("  dmcontrol=0x%08x", dmcontrol);
		LOG_ERROR("  dmstatus =0x%08x", dmstatus);
		abort();
	}

	dmcontrol = set_field(dmcontrol, DMI_DMCONTROL_HALTREQ, 0);
	dmi_write(target, DMI_DMCONTROL, dmcontrol);
}

static void riscv013_resume_current_hart(struct target *target)
{
	return riscv013_step_or_resume_current_hart(target, false);
}

static void riscv013_step_current_hart(struct target *target)
{
	return riscv013_step_or_resume_current_hart(target, true);
}

static void riscv013_on_resume(struct target *target)
{
	return riscv013_on_step_or_resume(target, false);
}

static void riscv013_on_step(struct target *target)
{
	return riscv013_on_step_or_resume(target, true);
}

static void riscv013_on_halt(struct target *target)
{
}

static bool riscv013_is_halted(struct target *target)
{
	uint32_t dmstatus = dmi_read(target, DMI_DMSTATUS);
	if (get_field(dmstatus, DMI_DMSTATUS_ANYUNAVAIL))
		LOG_ERROR("hart %d is unavailiable", riscv_current_hartid(target));
	if (get_field(dmstatus, DMI_DMSTATUS_ANYNONEXISTENT))
		LOG_ERROR("hart %d doesn't exist", riscv_current_hartid(target));
	return get_field(dmstatus, DMI_DMSTATUS_ALLHALTED);
}

static enum riscv_halt_reason riscv013_halt_reason(struct target *target)
{
	uint64_t dcsr = riscv_get_register(target, GDB_REGNO_DCSR);
	switch (get_field(dcsr, CSR_DCSR_CAUSE)) {
	case CSR_DCSR_CAUSE_SWBP:
	case CSR_DCSR_CAUSE_TRIGGER:
		return RISCV_HALT_BREAKPOINT;
	case CSR_DCSR_CAUSE_STEP:
		return RISCV_HALT_SINGLESTEP;
	case CSR_DCSR_CAUSE_DEBUGINT:
	case CSR_DCSR_CAUSE_HALT:
		return RISCV_HALT_INTERRUPT;
	}

	LOG_ERROR("Unknown DCSR cause field: %x", (int)get_field(dcsr, CSR_DCSR_CAUSE));
	LOG_ERROR("  dcsr=0x%016lx", (long)dcsr);
	abort();
}

void riscv013_write_debug_buffer(struct target *target, unsigned index, riscv_insn_t data)
{
	RISCV013_INFO(info);
	if (index >= info->progbufsize)
		return dmi_write(target, DMI_DATA0 + index - info->progbufsize, data);
	return dmi_write(target, DMI_PROGBUF0 + index, data);
}

riscv_insn_t riscv013_read_debug_buffer(struct target *target, unsigned index)
{
	RISCV013_INFO(info);
	if (index >= info->progbufsize)
		return dmi_read(target, DMI_DATA0 + index - info->progbufsize);
	return dmi_read(target, DMI_PROGBUF0 + index);
}

int riscv013_execute_debug_buffer(struct target *target)
{
	uint32_t run_program = 0;
	run_program = set_field(run_program, AC_ACCESS_REGISTER_SIZE, 2);
	run_program = set_field(run_program, AC_ACCESS_REGISTER_POSTEXEC, 1);
	run_program = set_field(run_program, AC_ACCESS_REGISTER_TRANSFER, 0);
	run_program = set_field(run_program, AC_ACCESS_REGISTER_REGNO, 0x1000);

	return execute_abstract_command(target, run_program);
}

void riscv013_fill_dmi_write_u64(struct target *target, char *buf, int a, uint64_t d)
{
	RISCV013_INFO(info);
	buf_set_u64((unsigned char *)buf, DTM_DMI_OP_OFFSET, DTM_DMI_OP_LENGTH, DMI_OP_WRITE);
	buf_set_u64((unsigned char *)buf, DTM_DMI_DATA_OFFSET, DTM_DMI_DATA_LENGTH, d);
	buf_set_u64((unsigned char *)buf, DTM_DMI_ADDRESS_OFFSET, info->abits, a);
}

void riscv013_fill_dmi_read_u64(struct target *target, char *buf, int a)
{
	RISCV013_INFO(info);
	buf_set_u64((unsigned char *)buf, DTM_DMI_OP_OFFSET, DTM_DMI_OP_LENGTH, DMI_OP_READ);
	buf_set_u64((unsigned char *)buf, DTM_DMI_DATA_OFFSET, DTM_DMI_DATA_LENGTH, 0);
	buf_set_u64((unsigned char *)buf, DTM_DMI_ADDRESS_OFFSET, info->abits, a);
}

void riscv013_fill_dmi_nop_u64(struct target *target, char *buf)
{
	RISCV013_INFO(info);
	buf_set_u64((unsigned char *)buf, DTM_DMI_OP_OFFSET, DTM_DMI_OP_LENGTH, DMI_OP_NOP);
	buf_set_u64((unsigned char *)buf, DTM_DMI_DATA_OFFSET, DTM_DMI_DATA_LENGTH, 0);
	buf_set_u64((unsigned char *)buf, DTM_DMI_ADDRESS_OFFSET, info->abits, 0);
}

int riscv013_dmi_write_u64_bits(struct target *target)
{
	RISCV013_INFO(info);
	return info->abits + DTM_DMI_DATA_LENGTH + DTM_DMI_OP_LENGTH;
}

/* Helper Functions. */
static void riscv013_on_step_or_resume(struct target *target, bool step)
{
	struct riscv_program program;
	riscv_program_init(&program, target);
	riscv_program_fence_i(&program);
	if (riscv_program_exec(&program, target) != ERROR_OK)
		LOG_ERROR("Unable to execute fence.i");

	/* We want to twiddle some bits in the debug CSR so debugging works. */
	uint64_t dcsr = riscv_get_register(target, GDB_REGNO_DCSR);
	dcsr = set_field(dcsr, CSR_DCSR_STEP, step);
	dcsr = set_field(dcsr, CSR_DCSR_EBREAKM, 1);
	dcsr = set_field(dcsr, CSR_DCSR_EBREAKS, 1);
	dcsr = set_field(dcsr, CSR_DCSR_EBREAKU, 1);
	riscv_set_register(target, GDB_REGNO_DCSR, dcsr);
}

static void riscv013_step_or_resume_current_hart(struct target *target, bool step)
{
	RISCV_INFO(r);
	LOG_DEBUG("resuming hart %d (for step?=%d)", r->current_hartid, step);
	assert(riscv_is_halted(target));

	struct riscv_program program;
	riscv_program_init(&program, target);
	riscv_program_fence_i(&program);
	if (riscv_program_exec(&program, target) != ERROR_OK)
		abort();

	/* Issue the resume command, and then wait for the current hart to resume. */
	uint32_t dmcontrol = dmi_read(target, DMI_DMCONTROL);
	dmcontrol = set_field(dmcontrol, DMI_DMCONTROL_RESUMEREQ, 1);
	dmi_write(target, DMI_DMCONTROL, dmcontrol);

	for (size_t i = 0; i < 256; ++i) {
		usleep(10);
		uint32_t dmstatus = dmi_read(target, DMI_DMSTATUS);
		if (get_field(dmstatus, DMI_DMSTATUS_ALLRESUMEACK) == 0)
			continue;
		if (step && get_field(dmstatus, DMI_DMSTATUS_ALLHALTED) == 0)
			continue;

		dmcontrol = set_field(dmcontrol, DMI_DMCONTROL_RESUMEREQ, 0);
		dmi_write(target, DMI_DMCONTROL, dmcontrol);
		return;
	}

	uint32_t dmstatus = dmi_read(target, DMI_DMSTATUS);
	dmcontrol = dmi_read(target, DMI_DMCONTROL);
	LOG_ERROR("unable to resume hart %d", r->current_hartid);
	LOG_ERROR("  dmcontrol=0x%08x", dmcontrol);
	LOG_ERROR("  dmstatus =0x%08x", dmstatus);

	if (step) {
		LOG_ERROR("  was stepping, halting");
		riscv013_halt_current_hart(target);
		return;
	}

	abort();
}

#if 0
riscv_addr_t riscv013_progbuf_addr(struct target *target)
{
	RISCV013_INFO(info);
	assert(info->progbuf_addr != -1);
	return info->progbuf_addr;
}
#endif

#if 0
riscv_addr_t riscv013_data_size(struct target *target)
{
	RISCV013_INFO(info);
	if (info->data_size == -1) {
		uint32_t acs = dmi_read(target, DMI_HARTINFO);
		info->data_size = get_field(acs, DMI_HARTINFO_DATASIZE);
	}
	return info->data_size;
}
#endif

#if 0
riscv_addr_t riscv013_data_addr(struct target *target)
{
	RISCV013_INFO(info);
	if (info->data_addr == -1) {
		uint32_t acs = dmi_read(target, DMI_HARTINFO);
		info->data_addr = get_field(acs, DMI_HARTINFO_DATAACCESS) ? get_field(acs, DMI_HARTINFO_DATAADDR) : 0;
	}
	return info->data_addr;
}
#endif

#if 0
int riscv013_debug_buffer_register(struct target *target, riscv_addr_t addr)
{
	if (addr >= riscv013_data_addr(target))
		return DMI_DATA0 + (addr - riscv013_data_addr(target)) / 4;
	else
		return DMI_PROGBUF0 + (addr - riscv013_progbuf_addr(target)) / 4;
}
#endif

void riscv013_clear_abstract_error(struct target *target)
{
	// Wait for busy to go away.
	time_t start = time(NULL);
	uint32_t abstractcs = dmi_read(target, DMI_ABSTRACTCS);
	while (get_field(abstractcs, DMI_ABSTRACTCS_BUSY)) {
		abstractcs = dmi_read(target, DMI_ABSTRACTCS);

		if (time(NULL) - start > riscv_command_timeout_sec) {
			LOG_ERROR("abstractcs.busy is not going low after %d seconds "
					"(abstractcs=0x%x). The target is either really slow or "
					"broken. You could increase the timeout with riscv "
					"set_reset_timeout_sec.",
					riscv_command_timeout_sec, abstractcs);
			break;
		}
	}
	// Clear the error status.
	dmi_write(target, DMI_ABSTRACTCS, abstractcs & DMI_ABSTRACTCS_CMDERR);
}
