
// uncomment to enable printf in CKB-VM
//#define CKB_C_STDLIB_PRINTF
//#include <stdio.h>

// it's used by blockchain-api2.h, the behavior when panic
#ifndef MOL2_EXIT
#define MOL2_EXIT ckb_exit
#endif
int ckb_exit(signed char);

#include <stdbool.h>
#include <string.h>

#include "blake2b.h"
#include "blockchain-api2.h"
#include "ckb_consts.h"

#if defined(CKB_USE_SIM)
#include <stdio.h>

#include "ckb_syscall_xudt_sim.h"
#define xudt_printf printf
#else
// it will be re-defined in ckb_dlfcn.h
#undef MAX
#undef MIN
#include "ckb_dlfcn.h"
#include "ckb_syscalls.h"
#define xudt_printf(x, ...) (void)0
#endif

#define BLAKE160_SIZE 20
#define SCRIPT_SIZE 32768
#define RAW_EXTENSION_SIZE 65536
#define EXPORTED_FUNC_NAME "validate"
// here we reserve a lot of memory for dynamic libraries. The enhanced owner
// mode may also checked via dynamic library. It might consume much memory, e.g.
// precomputed table (about 1 M) in secp256k1
#define MAX_CODE_SIZE (1024 * 1800)
#define FLAGS_SIZE 4
#define MAX_LOCK_SCRIPT_HASH_COUNT 2048

#define OWNER_MODE_INPUT_TYPE_MASK 0x80000000
#define OWNER_MODE_OUTPUT_TYPE_MASK 0x40000000
#define OWNER_MODE_INPUT_LOCK_NOT_MASK 0x20000000
#define OWNER_MODE_MASK                                                        \
  (OWNER_MODE_INPUT_TYPE_MASK | OWNER_MODE_OUTPUT_TYPE_MASK |                  \
   OWNER_MODE_INPUT_LOCK_NOT_MASK)

#include "rce.h"

// global variables, type definitions, etc

// We will leverage gcc's 128-bit integer extension here for number crunching.
typedef unsigned __int128 uint128_t;

uint8_t g_script[SCRIPT_SIZE] = {0};
uint8_t g_raw_extension_data[RAW_EXTENSION_SIZE] = {0};
WitnessArgsType g_witness_args;

uint8_t g_code_buff[MAX_CODE_SIZE] __attribute__((aligned(RISCV_PGSIZE)));
uint32_t g_code_used = 0;

/*
is_owner_mode indicates if current xUDT is unlocked via owner mode(as
described by sUDT), extension_index refers to the index of current extension in
the ScriptVec structure. args and args_length are set to the script args
included in Script structure of current extension script.

If this function returns 0, the validation for current extension script is
consider successful.
 */
typedef int (*ValidateFuncType)(int is_owner_mode, size_t extension_index,
                                const uint8_t *args, size_t args_len);

typedef enum XUDTFlags {
  XUDTFlagsPlain = 0,
  XUDTFlagsInArgs = 1,
  XUDTFlagsInWitness = 2,
} XUDTFlags;

typedef enum XUDTValidateFuncCategory {
  CateNormal = 0, // normal extension script
  CateRce = 1,    // Regulation Compliance Extension
} XUDTValidateFuncCategory;

uint8_t RCE_HASH[32] = {1};

// functions
int load_validate_func(uint8_t *g_code_buff, uint32_t *g_code_used,
                       const uint8_t *hash, uint8_t hash_type,
                       ValidateFuncType *func, XUDTValidateFuncCategory *cat) {
  int err = 0;
  void *handle = NULL;
  size_t consumed_size = 0;

  if (memcmp(RCE_HASH, hash, 32) == 0 && hash_type == 1) {
    *cat = CateRce;
    *func = rce_validate;
    return 0;
  }

  CHECK2(MAX_CODE_SIZE > *g_code_used, ERROR_NOT_ENOUGH_BUFF);
  err = ckb_dlopen2(hash, hash_type, &g_code_buff[*g_code_used],
                    MAX_CODE_SIZE - *g_code_used, &handle, &consumed_size);
  CHECK(err);
  CHECK2(handle != NULL, ERROR_CANT_LOAD_LIB);
  ASSERT(consumed_size % RISCV_PGSIZE == 0);
  *g_code_used += consumed_size;

  *func = (ValidateFuncType)ckb_dlsym(handle, EXPORTED_FUNC_NAME);
  CHECK2(*func != NULL, ERROR_CANT_FIND_SYMBOL);

  *cat = CateNormal;
  err = 0;
exit:
  return err;
}

int verify_script_vec(uint8_t *ptr, uint32_t size, uint32_t *real_size) {
  int err = 0;

  CHECK2(size >= MOL_NUM_T_SIZE, ERROR_INVALID_MOL_FORMAT);
  mol_num_t full_size = mol_unpack_number(ptr);
  *real_size = full_size;
  CHECK2(*real_size <= size, ERROR_INVALID_MOL_FORMAT);
  err = 0;
exit:
  return err;
}

static uint32_t read_from_witness(uintptr_t arg[], uint8_t *ptr, uint32_t len,
                                  uint32_t offset) {
  int err;
  uint64_t output_len = len;
  err = ckb_load_witness(ptr, &output_len, offset, arg[0], arg[1]);
  if (err != 0) {
    return 0;
  }
  if (output_len > len) {
    return len;
  } else {
    return output_len;
  }
}

uint8_t g_witness_data_source[DEFAULT_DATA_SOURCE_LENGTH];
// due to the "static" data (s_witness_data_source), the "WitnessArgsType" is a
// singleton. note: mol2_data_source_t consumes a lot of memory due to the
// "cache" field (default 2K)
int make_cursor_from_witness(WitnessArgsType *witness, bool *use_input_type) {
  int err = 0;
  uint64_t witness_len = 0;
  // at the beginning of the transactions including RCE,
  // there is no "witness" in CKB_SOURCE_GROUP_INPUT
  // here we use the first witness of CKB_SOURCE_GROUP_OUTPUT
  // same logic is applied to rce_validator
  size_t source = CKB_SOURCE_GROUP_INPUT;
  err = ckb_load_witness(NULL, &witness_len, 0, 0, source);
  if (err == CKB_INDEX_OUT_OF_BOUND) {
    source = CKB_SOURCE_GROUP_OUTPUT;
    err = ckb_load_witness(NULL, &witness_len, 0, 0, source);
    *use_input_type = false;
  } else {
    *use_input_type = true;
  }
  CHECK(err);
  CHECK2(witness_len > 0, ERROR_INVALID_MOL_FORMAT);

  mol2_cursor_t cur;

  cur.offset = 0;
  cur.size = witness_len;

  mol2_data_source_t *ptr = (mol2_data_source_t *)g_witness_data_source;

  ptr->read = read_from_witness;
  ptr->total_size = witness_len;
  // pass index and source as args
  ptr->args[0] = 0;
  ptr->args[1] = source;

  ptr->cache_size = 0;
  ptr->start_point = 0;
  ptr->max_cache_size = MAX_CACHE_SIZE;
  cur.data_source = ptr;

  *witness = make_WitnessArgs(&cur);

  err = 0;
exit:
  return err;
}

int get_extension_data(uint32_t index, uint8_t *buff, uint32_t buff_len,
                       uint32_t *out_len) {
  int err = 0;
  bool use_input_type = true;
  err = make_cursor_from_witness(&g_witness_args, &use_input_type);
  CHECK(err);

  BytesOptType input;
  if (use_input_type)
    input = g_witness_args.t->input_type(&g_witness_args);
  else
    input = g_witness_args.t->output_type(&g_witness_args);

  CHECK2(!input.t->is_none(&input), ERROR_INVALID_MOL_FORMAT);

  mol2_cursor_t bytes = input.t->unwrap(&input);
  // convert Bytes to XudtWitnessInputType
  XudtWitnessInputType witness_input = make_XudtWitnessInput(&bytes);
  BytesVecType extension_data_vec =
      witness_input.t->extension_data(&witness_input);

  bool existing = false;
  mol2_cursor_t extension_data =
      extension_data_vec.t->get(&extension_data_vec, index, &existing);
  CHECK2(existing, ERROR_INVALID_MOL_FORMAT);
  CHECK2(buff_len >= extension_data.size, ERROR_INVALID_MOL_FORMAT);

  *out_len = mol2_read_at(&extension_data, buff, buff_len);
  CHECK2(*out_len == extension_data.size, ERROR_INVALID_MOL_FORMAT);

  err = 0;
exit:
  return err;
}

int get_owner_script(uint8_t *buff, uint32_t buff_len, uint32_t *out_len) {
  int err = 0;
  bool use_input_type = true;
  err = make_cursor_from_witness(&g_witness_args, &use_input_type);
  CHECK(err);
  BytesOptType input = use_input_type
                           ? g_witness_args.t->input_type(&g_witness_args)
                           : g_witness_args.t->output_type(&g_witness_args);
  CHECK2(!input.t->is_none(&input), ERROR_INVALID_MOL_FORMAT);

  mol2_cursor_t bytes = input.t->unwrap(&input);
  // convert Bytes to XudtWitnessInputType
  XudtWitnessInputType witness_input = make_XudtWitnessInput(&bytes);
  ScriptOptType owner_script = witness_input.t->owner_script(&witness_input);
  CHECK2(!owner_script.t->is_none(&owner_script), ERROR_INVALID_MOL_FORMAT);
  ScriptType owner_script2 = owner_script.t->unwrap(&owner_script);
  *out_len = mol2_read_at(&owner_script2.cur, buff, buff_len);
  CHECK2(*out_len == owner_script2.cur.size, ERROR_INVALID_MOL_FORMAT);

  err = 0;
exit:
  return err;
}

// the *var_len may be bigger than real length of raw extension data
int load_raw_extension_data(uint8_t **var_data, uint32_t *var_len) {
  int err = 0;
  bool use_input_type = true;
  err = make_cursor_from_witness(&g_witness_args, &use_input_type);
  CHECK(err);

  BytesOptType input;
  if (use_input_type) {
    input = g_witness_args.t->input_type(&g_witness_args);
  } else {
    input = g_witness_args.t->output_type(&g_witness_args);
  }

  CHECK2(!input.t->is_none(&input), ERROR_INVALID_MOL_FORMAT);

  struct mol2_cursor_t bytes = input.t->unwrap(&input);
  // convert Bytes to XudtWitnessInputType
  XudtWitnessInputType witness_input = make_XudtWitnessInput(&bytes);
  ScriptVecOptType script_vec =
      witness_input.t->raw_extension_data(&witness_input);

  uint32_t read_len =
      mol2_read_at(&script_vec.cur, g_raw_extension_data, RAW_EXTENSION_SIZE);
  CHECK2(read_len == script_vec.cur.size, ERROR_INVALID_MOL_FORMAT);

  *var_data = g_raw_extension_data;
  *var_len = read_len;

  err = 0;
exit:
  return err;
}

int check_owner_mode(size_t source, size_t field, mol_seg_t args_bytes_seg,
                     int *owner_mode) {
  int err = 0;
  size_t i = 0;
  uint8_t buffer[BLAKE2B_BLOCK_SIZE];

  while (1) {
    uint64_t len = BLAKE2B_BLOCK_SIZE;
    err = ckb_checked_load_cell_by_field(buffer, &len, 0, i, source, field);
    if (err == CKB_INDEX_OUT_OF_BOUND) {
      err = 0;
      break;
    }
    if (err == CKB_ITEM_MISSING) {
      i += 1;
      err = 0;
      continue;
    }
    CHECK(err);
    if (args_bytes_seg.size >= BLAKE2B_BLOCK_SIZE &&
        memcmp(buffer, args_bytes_seg.ptr, BLAKE2B_BLOCK_SIZE) == 0) {
      *owner_mode = 1;
      break;
    }
    i += 1;
  }

exit:
  return err;
}

int check_enhanced_owner_mode(int *owner_mode) {
  int err = 0;
  uint8_t owner_script[SCRIPT_SIZE];
  uint32_t owner_script_len = 0;
  uint8_t owner_script_hash[BLAKE2B_BLOCK_SIZE] = {0};

  err = get_owner_script(owner_script, SCRIPT_SIZE, &owner_script_len);
  CHECK(err);

  err = blake2b(owner_script_hash, BLAKE2B_BLOCK_SIZE, owner_script,
                owner_script_len, NULL, 0);
  CHECK2(err == 0, ERROR_BLAKE2B_ERROR);

  // get 32 bytes hash from args and compare it to owner script hash
  {
    uint64_t len = SCRIPT_SIZE;
    int ret = ckb_checked_load_script(g_script, &len, 0);
    CHECK(ret);
    CHECK2(len <= SCRIPT_SIZE, ERROR_SCRIPT_TOO_LONG);

    mol_seg_t script_seg;
    script_seg.ptr = g_script;
    script_seg.size = len;

    mol_errno mol_err = MolReader_Script_verify(&script_seg, false);
    CHECK2(mol_err == MOL_OK, ERROR_ENCODING);

    mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
    mol_seg_t args_bytes_seg = MolReader_Bytes_raw_bytes(&args_seg);
    CHECK2(args_bytes_seg.size >= BLAKE2B_BLOCK_SIZE, ERROR_ARGUMENTS_LEN);

    if (memcmp(owner_script_hash, args_bytes_seg.ptr, BLAKE2B_BLOCK_SIZE) !=
        0) {
      CHECK2(false, ERROR_HASH_MISMATCHED);
    }
  }

  // execute owner script
  mol_seg_t owner_script_seg = {.ptr = owner_script, .size = owner_script_len};
  mol_errno mol_err = MolReader_Script_verify(&owner_script_seg, false);
  CHECK2(mol_err == MOL_OK, ERROR_ENCODING);
  mol_seg_t code_hash = MolReader_Script_get_code_hash(&owner_script_seg);
  mol_seg_t hash_type = MolReader_Script_get_hash_type(&owner_script_seg);

  mol_seg_t owner_args_seg = MolReader_Script_get_args(&owner_script_seg);
  mol_seg_t owner_args_bytes_seg = MolReader_Bytes_raw_bytes(&owner_args_seg);

  ValidateFuncType func = NULL;
  XUDTValidateFuncCategory cat = CateNormal;
  err = load_validate_func(g_code_buff, &g_code_used, code_hash.ptr,
                           *(uint8_t *)hash_type.ptr, &func, &cat);
  CHECK(err);

  err = func(0, 0, owner_args_bytes_seg.ptr, owner_args_bytes_seg.size);
  CHECK(err);
  *owner_mode = 1;

exit:
  return err;
}

// *var_data will point to "Raw Extension Data", which can be in args or witness
// *var_data will refer to a memory location of g_script or g_raw_extension_data
int parse_args(int *owner_mode, XUDTFlags *flags, uint8_t **var_data,
               uint32_t *var_len, uint8_t *hashes, uint32_t *hashes_count) {
  int err = 0;
  bool owner_mode_for_input_type = false;
  bool owner_mode_for_output_type = false;
  // default is on
  bool owner_mode_for_input_lock = true;

  uint64_t len = SCRIPT_SIZE;
  int ret = ckb_checked_load_script(g_script, &len, 0);
  CHECK(ret);
  CHECK2(len <= SCRIPT_SIZE, ERROR_SCRIPT_TOO_LONG);

  mol_seg_t script_seg;
  script_seg.ptr = g_script;
  script_seg.size = len;

  mol_errno mol_err = MolReader_Script_verify(&script_seg, false);
  CHECK2(mol_err == MOL_OK, ERROR_ENCODING);

  mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
  mol_seg_t args_bytes_seg = MolReader_Bytes_raw_bytes(&args_seg);
  CHECK2(args_bytes_seg.size >= BLAKE2B_BLOCK_SIZE, ERROR_ARGUMENTS_LEN);

  if (args_bytes_seg.size >= (FLAGS_SIZE + BLAKE2B_BLOCK_SIZE)) {
    uint32_t val = *(uint32_t *)(args_bytes_seg.ptr + BLAKE2B_BLOCK_SIZE);
    if (val & OWNER_MODE_INPUT_TYPE_MASK) {
      owner_mode_for_input_type = true;
    }
    if (val & OWNER_MODE_OUTPUT_TYPE_MASK) {
      owner_mode_for_output_type = true;
    }
    if (val & OWNER_MODE_INPUT_LOCK_NOT_MASK) {
      owner_mode_for_input_lock = false;
    }
  }

  *hashes_count = 0;

  // collect hashes
  size_t i = 0;
  while (1) {
    uint8_t buffer[BLAKE2B_BLOCK_SIZE];
    uint64_t len2 = BLAKE2B_BLOCK_SIZE;
    ret = ckb_checked_load_cell_by_field(buffer, &len2, 0, i, CKB_SOURCE_INPUT,
                                         CKB_CELL_FIELD_LOCK_HASH);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    CHECK(ret);
    CHECK2(*hashes_count < MAX_LOCK_SCRIPT_HASH_COUNT, ERROR_TOO_MANY_LOCK);

    memcpy(&hashes[(*hashes_count) * BLAKE2B_BLOCK_SIZE], buffer,
           BLAKE2B_BLOCK_SIZE);
    *hashes_count += 1;
    i += 1;
  }

  *owner_mode = 0;

  if (owner_mode_for_input_lock && *owner_mode == 0) {
    err = check_owner_mode(CKB_SOURCE_INPUT, CKB_CELL_FIELD_LOCK_HASH,
                           args_bytes_seg, owner_mode);
    CHECK(err);
  }

  if (owner_mode_for_input_type && *owner_mode == 0) {
    err = check_owner_mode(CKB_SOURCE_INPUT, CKB_CELL_FIELD_TYPE_HASH,
                           args_bytes_seg, owner_mode);
    CHECK(err);
  }
  if (owner_mode_for_output_type && *owner_mode == 0) {
    err = check_owner_mode(CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_TYPE_HASH,
                           args_bytes_seg, owner_mode);
    CHECK(err);
  }

  // parse xUDT args
  if (args_bytes_seg.size < (FLAGS_SIZE + BLAKE2B_BLOCK_SIZE)) {
    *var_data = NULL;
    *var_len = 0;
    *flags = XUDTFlagsPlain;
  } else {
    uint32_t temp_flags =
        (*(uint32_t *)(args_bytes_seg.ptr + BLAKE2B_BLOCK_SIZE)) &
        ~OWNER_MODE_MASK;
    if (temp_flags == XUDTFlagsPlain) {
      *flags = XUDTFlagsPlain;
    } else if (temp_flags == XUDTFlagsInArgs) {
      uint32_t real_size = 0;
      *flags = XUDTFlagsInArgs;
      *var_len = args_bytes_seg.size - BLAKE2B_BLOCK_SIZE - FLAGS_SIZE;
      *var_data = args_bytes_seg.ptr + BLAKE2B_BLOCK_SIZE + FLAGS_SIZE;

      err = verify_script_vec(*var_data, *var_len, &real_size);
      CHECK(err);
      // note, it's different than "flag = 2"
      CHECK2(real_size == *var_len, ERROR_INVALID_ARGS_FORMAT);
    } else if (temp_flags == XUDTFlagsInWitness) {
      *flags = XUDTFlagsInWitness;
      uint32_t hash_size =
          args_bytes_seg.size - BLAKE2B_BLOCK_SIZE - FLAGS_SIZE;
      CHECK2(hash_size == BLAKE160_SIZE, ERROR_INVALID_FLAG);

      err = load_raw_extension_data(var_data, var_len);
      CHECK(err);
      CHECK2(var_len > 0, ERROR_INVALID_MOL_FORMAT);
      // verify the hash
      uint8_t hash[BLAKE2B_BLOCK_SIZE] = {0};
      uint8_t *blake160_hash =
          args_bytes_seg.ptr + BLAKE2B_BLOCK_SIZE + FLAGS_SIZE;
      err = blake2b(hash, BLAKE2B_BLOCK_SIZE, *var_data, *var_len, NULL, 0);
      CHECK2(err == 0, ERROR_BLAKE2B_ERROR);
      CHECK2(memcmp(blake160_hash, hash, BLAKE160_SIZE) == 0,
             ERROR_HASH_MISMATCHED);
    } else {
      CHECK2(false, ERROR_INVALID_FLAG);
    }
  }
  err = 0;
exit:
  return err;
}

// copied from simple_udt.c
int simple_udt(int owner_mode) {
  int ret = 0;
  // When the owner mode is not enabled, however, we will then need to ensure
  // the sum of all input tokens is not smaller than the sum of all output
  // tokens. First, let's loop through all input cells containing current UDTs,
  // and gather the sum of all input tokens.
  uint128_t input_amount = 0;
  size_t input_index = 0;
  uint64_t len = 0;
  while (1) {
    uint128_t current_amount = 0;
    len = 16;
    ret = ckb_load_cell_data((uint8_t *)&current_amount, &len, 0, input_index,
                             CKB_SOURCE_GROUP_INPUT);
    // When `CKB_INDEX_OUT_OF_BOUND` is reached, we know we have iterated
    // through all cells of current type.
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len < 16) {
      return ERROR_ENCODING;
    }
    input_amount += current_amount;
    // Like any serious smart contract out there, we will need to check for
    // overflows.
    if (input_amount < current_amount) {
      return ERROR_OVERFLOWING;
    }
    input_index += 1;
  }

  // With the sum of all input UDT tokens gathered, let's now iterate through
  // output cells to grab the sum of all output UDT tokens.
  uint128_t output_amount = 0;
  size_t output_index = 0;
  while (1) {
    uint128_t current_amount = 0;
    len = 16;
    // Similar to the above code piece, we are also looping through output cells
    // with the same script as current running script here by using
    // `CKB_SOURCE_GROUP_OUTPUT`.
    ret = ckb_load_cell_data((uint8_t *)&current_amount, &len, 0, output_index,
                             CKB_SOURCE_GROUP_OUTPUT);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len < 16) {
      return ERROR_ENCODING;
    }
    output_amount += current_amount;
    // Like any serious smart contract out there, we will need to check for
    // overflows.
    if (output_amount < current_amount) {
      return ERROR_OVERFLOWING;
    }
    output_index += 1;
  }

  if (input_amount != 0 && output_amount != 0 ) {
      size_t i = 0;
      while (1) {
        uint128_t input_amount_per_cell = 0;
        len = 16;
        // we are also looping through input cells
        ret = ckb_load_cell_data((uint8_t *)&input_amount_per_cell, &len, 0, i,
                                CKB_SOURCE_GROUP_INPUT);
        if (ret == CKB_INDEX_OUT_OF_BOUND) {
          break;
        }
        if (ret != CKB_SUCCESS) {
          return ret;
        }
        if (len < 16) {
          return ERROR_ENCODING;
        }

        uint128_t output_amount_per_cell = 0;
        // Similar to the above code piece, we are also looping through output cells
        ret = ckb_load_cell_data((uint8_t *)&output_amount_per_cell, &len, 0, i,
                                CKB_SOURCE_GROUP_OUTPUT);
        if (ret == CKB_INDEX_OUT_OF_BOUND) {
          break;
        }
        if (ret != CKB_SUCCESS) {
          return ret;
        }
        if (len < 16) {
          return ERROR_ENCODING;
        }
        
        if (output_amount_per_cell != input_amount_per_cell) {
          return ERROR_AMOUNT;
        }
        i += 1;
      }
  }

  // When both value are gathered, we can perform the final check here to
  // prevent non-authorized token issuance.
  if ((input_amount < output_amount) && !owner_mode) {
    return ERROR_AMOUNT;
  }
  return CKB_SUCCESS;
}

// If the extension script is identical to a lock script of one input cell in
// current transaction, we consider the extension script to be already
// validated, no additional check is needed for current extension
int is_extension_script_validated(mol_seg_t extension_script,
                                  uint8_t *input_lock_script_hash,
                                  uint32_t input_lock_script_hash_count) {
  int err = 0;
  uint8_t hash[BLAKE2B_BLOCK_SIZE];
  err = blake2b(hash, BLAKE2B_BLOCK_SIZE, extension_script.ptr,
                extension_script.size, NULL, 0);
  CHECK2(err == 0, ERROR_BLAKE2B_ERROR);

  for (uint32_t i = 0; i < input_lock_script_hash_count; i++) {
    if (memcmp(&input_lock_script_hash[i * BLAKE2B_BLOCK_SIZE], hash,
               BLAKE2B_BLOCK_SIZE) == 0) {
      return 0;
    }
  }
  err = ERROR_NOT_VALIDATED;
exit:
  return err;
}

#ifdef CKB_USE_SIM
int simulator_main() {
#else
int main() {
#endif
  int err = 0;
  int owner_mode = 0;
  uint8_t *raw_extension_data = NULL;
  uint32_t raw_extension_len = 0;
  XUDTFlags flags = XUDTFlagsPlain;
  uint8_t
      input_lock_script_hashes[MAX_LOCK_SCRIPT_HASH_COUNT * BLAKE2B_BLOCK_SIZE];
  uint32_t input_lock_script_hash_count = 0;
  err = parse_args(&owner_mode, &flags, &raw_extension_data, &raw_extension_len,
                   input_lock_script_hashes, &input_lock_script_hash_count);
  CHECK(err);
  CHECK2(owner_mode == 1 || owner_mode == 0, ERROR_INVALID_ARGS_FORMAT);
  // check enhanced mode here
  if (!owner_mode) {
    check_enhanced_owner_mode(&owner_mode);
    // don't need to check the return result from this function
    // if failed, owner mode is still false
  }

  if (flags != XUDTFlagsPlain) {
    CHECK2(raw_extension_data != NULL, ERROR_INVALID_ARGS_FORMAT);
    CHECK2(raw_extension_len > 0, ERROR_INVALID_ARGS_FORMAT);
  }
  err = simple_udt(owner_mode);
  if (err != 0) {
    goto exit;
  }

  if (flags == XUDTFlagsPlain) {
    err = 0;
    goto exit;
  }

  mol_seg_t raw_extension_seg = {0};
  raw_extension_seg.ptr = raw_extension_data;
  raw_extension_seg.size = raw_extension_len;
  CHECK2(MolReader_ScriptVec_verify(&raw_extension_seg, true) == MOL_OK,
         ERROR_INVALID_ARGS_FORMAT);
  uint32_t size = MolReader_ScriptVec_length(&raw_extension_seg);
  for (uint32_t i = 0; i < size; i++) {
    ValidateFuncType func;
    mol_seg_res_t res = MolReader_ScriptVec_get(&raw_extension_seg, i);
    CHECK2(res.errno == 0, ERROR_INVALID_MOL_FORMAT);
    CHECK2(MolReader_Script_verify(&res.seg, false) == MOL_OK,
           ERROR_INVALID_MOL_FORMAT);

    mol_seg_t code_hash = MolReader_Script_get_code_hash(&res.seg);
    mol_seg_t hash_type = MolReader_Script_get_hash_type(&res.seg);
    mol_seg_t args = MolReader_Script_get_args(&res.seg);

    uint8_t hash_type2 = *((uint8_t *)hash_type.ptr);
    XUDTValidateFuncCategory cat = CateNormal;
    err = load_validate_func(g_code_buff, &g_code_used, code_hash.ptr,
                             hash_type2, &func, &cat);
    CHECK(err);
    // RCE is with high priority, must be checked
    if (cat != CateRce) {
      int err2 = is_extension_script_validated(
          res.seg, input_lock_script_hashes, input_lock_script_hash_count);
      if (err2 == 0) {
        continue;
      }
    }
    mol_seg_t args_raw_bytes = MolReader_Bytes_raw_bytes(&args);

    err = func(owner_mode, i, args_raw_bytes.ptr, args_raw_bytes.size);
    CHECK(err);
  }

  err = 0;
exit:
  return err;
}
