#include <cstdint>
#include <sstream>
#include <fstream>
#include <iostream>
#include <set>
#include <ctime> 
#include <boost/functional/hash.hpp>
#include "pin.H"
#include "regvalue_utils.h"
#include "EXCVATE_utils.h"
#include <sys/syscall.h>

#ifndef VERBOSE
#define VERBOSE 0
#endif

#define BV32_ONE     "( ( _ to_fp 8 24 ) #b00111111100000000000000000000000 )"
#define BV32_ABS     "( ( _ to_fp 8 24 ) #b01111111111111111111111111111111 )"
#define BV32_NEG     "( ( _ to_fp 8 24 ) #b10000000000000000000000000000000 )"
#define BV32_ZERO    "( ( _ to_fp 8 24 ) #b00000000000000000000000000000000 )"
#define BV32_ID      "( ( _ to_fp 8 24 ) #b11111111111111111111111111111111 )"
#define BV64_ONE     "( ( _ to_fp 8 24 ) #b0011111111110000000000000000000000000000000000000000000000000000 )"
#define BV64_ABS    "( ( _ to_fp 11 53 ) #b0111111111111111111111111111111111111111111111111111111111111111 )"
#define BV64_NEG    "( ( _ to_fp 11 53 ) #b1000000000000000000000000000000000000000000000000000000000000000 )"
#define BV64_ZERO   "( ( _ to_fp 11 53 ) #b0000000000000000000000000000000000000000000000000000000000000000 )"
#define BV64_ID     "( ( _ to_fp 11 53 ) #b1111111111111111111111111111111111111111111111111111111111111111 )"

// #define EVS_IN_INPUTS

using namespace std;

enum Mode {
    BASELINE_ONLY = 0,
    PRE_EV_OVERWRITE_BASELINE = 1,
    EV_OVERWRITE = 2,
};

// Globals
bool SYS_WRITE_INTERCEPT_FLAG=false;
uint32_t MAX_IO_VAR_BYTES;
uint8_t* IO_VAR_BUFFER_BASE_PTR;
uint32_t IO_VAR_BUFFER_OFFSET = 0;
uint32_t IO_VAR_EXTRA_SPACE = 0;
FuncStaticInfo* TARGETED_FUNC_STATIC_INFO;
FuncDynamicInfo* TARGETED_FUNC_DYNAMIC_INFO;
vector<string> ERROR_HANDLER_NAMES;
map<const size_t,vector<uint8_t>> SAVED_IO_VARS_MAP;
const pair<const size_t,vector<uint8_t>>* CURRENT_IO_VARS;
vector<uint8_t>::const_iterator CURRENT_IO_VARS_BYTES_IT;
AFUNPTR TARGETED_FUNCTION_PTR;
map<ADDRINT, map<ADDRINT, Instruction*>> INSTRUCTION_OBJ_MAP;
map<ADDRINT, string> ROUTINE_NAME_MAP;
ostringstream WRITE_BUFFER;
bool GO=false;
uint32_t MODE;
size_t EV_GENERATOR_HASH = 0;
int32_t EXIT_CODE = 0;
vector<ADDRINT> INJECTED_IO_VAR_PTRS;
vector<ADDRINT> CALL_STACK;
map<size_t,set<size_t>> IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP;
map<ADDRINT,map<ADDRINT, ADDRINT>> INSTRUCTION_EXECUTION_COUNTS;
boost::hash<vector<string>> EV_GENERATOR_HASH_FUNCTION;
struct timespec START_TIME;
double TIME_LIMIT;
CONTEXT* RETURN_CONTEXT = NULL;
uint32_t N_MISSES = 0;
uint32_t N_HITS = 0;
uint32_t N_EXIT0 = 0;
uint32_t N_EXIT1 = 0;
// set<string> SKIP;
map<string, uint64_t> SYM_VAR_COUNTERS;
set<string> ACTIVE_SYM_VAR_NAMES;

// Forward Declarations
void image_pass1( IMG img, void *v );
void image_pass2( IMG img, void *v );
void trace_pass( TRACE trace, VOID* v );
void error_handler_called();
void reset_and_run( CONTEXT* ctxt, const pair<const size_t,vector<uint8_t>>* io_vars );
void perform_analysis_loop( CONTEXT* ctxt );
void instrument_instruction( INS ins );
void process_EV_generator_reg( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT n_values, PINTOOL_REGISTER* write_register );
void intercept_stdout_and_stderr_writes( THREADID threadIndex, CONTEXT* ctxt, SYSCALL_STANDARD std, VOID* v );
void save_return_context( const CONTEXT* ctxt );
void check_for_timeout();
void print_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode );
void log_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode );
void inject_io_var( ADDRINT* actual_val_ptr, ADDRINT var_idx );
template <typename T>
vector<ADDRINT> check_for_EVs_mem( ADDRINT base_address, uint32_t n_values, uint32_t n_bytes, bool print=false );
template <typename T>
void post_call( T* return_value_ptr );
void pre_call( ADDRINT rtn_id );
void cmp_readVV( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg1_val, PINTOOL_REGISTER* r_reg2_val );
void cmp_readVM( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg_val, ADDRINT r_base_address );
void writeV_readV( ADDRINT rtn_id, ADDRINT ins_offset,  PINTOOL_REGISTER* r_reg_val );
void writeV_readM( ADDRINT rtn_id, ADDRINT ins_offset,  ADDRINT r_base_address );
void writeM_readV( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT w_mem_base_address,  PINTOOL_REGISTER* r_reg_val );
void writeV_readVM( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg_val, ADDRINT r_base_address );
void writeV_readVV( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg1_val, PINTOOL_REGISTER* r_reg2_val );
void get_string_values_reg_operand( PINTOOL_REGISTER* reg_val, Operand* reg_obj, vector<string>& operand_values, bool& read_symbolic );
void get_string_values_mem_operand( ADDRINT base_address, Operand* mem_obj, vector<string>& operand_values, bool& read_symbolic );
void process_w_reg_operands( Instruction* ins_obj_ptr, vector<string>& w_operands, vector<string>& r_operands1, vector<string>& r_operands2, bool& read_symbolic );
void process_w_mem_operands( Instruction* ins_obj_ptr, ADDRINT mem_base_address, vector<string>& w_operands, vector<string>& r_operands1, vector<string>& r_operands2, bool& read_symbolic );
string reg_to_sym_var_base_name( Operand* reg_obj, bool create_fresh );
string mem_to_sym_var_uniq_name( Operand* mem_obj, ADDRINT effective_address, bool create_fresh );
void add_smt_text( Instruction* ins_obj_ptr, const vector<string>& w_operands, const vector<string>& r_operands1, const vector<string>& r_operands2 );
void add_smt_text_helper( Instruction* ins_obj_ptr, string w_operand, string r_operand1, string r_operand2 );
string get_bitvector_string_reg( PINTOOL_REGISTER* reg, uint32_t value_idx, uint32_t n_bytes );
string get_bitvector_string_mem( ADDRINT address, uint32_t n_bytes );
string select( vector<string> src, uint32_t src_start_idx, bitset<64> ctrl, uint32_t ctrl_start_idx );
double get_double_value_reg( PINTOOL_REGISTER* reg, uint32_t value_idx, uint32_t n_bytes );
double get_double_value_mem( ADDRINT address, uint32_t n_bytes );
string get_relation( double value1, double value2, string r_operand1, string r_operand2 );

string get_relation( double value1, double value2, string r_operand1, string r_operand2 ){
    if ( isnan(value1) || isnan(value2) ){
        return "nan_comparison";
    }
    else if ( value1 > value2 ){
        return "fp.gt " + r_operand1 + " " + r_operand2;
    }
    else if ( value1 < value2 ){
        return "fp.lt " + r_operand1 + " " + r_operand2;
    }
    else if ( value1 == value2 ){
        return "fp.eq " + r_operand1 + " " + r_operand2;
    }
    else{
        assert(false);
    }
}

double get_double_value_reg( PINTOOL_REGISTER* reg, uint32_t value_idx, uint32_t n_bytes ){
    double value = 0;
    if ( n_bytes == 8 ){
        value = reg->dbl[value_idx];
    }
    else if ( n_bytes == 4 ){
        value = (double) reg->flt[value_idx];
    }
    return value;
}

double get_double_value_mem( ADDRINT address, uint32_t n_bytes ){
    double value = 0;
    if ( n_bytes == 8 ){
        value = *(reinterpret_cast<double*>(address));
    }
    else if ( n_bytes == 4 ){
        value = (double) *(reinterpret_cast<float*>(address));
    }
    return value;
}

void cmp_readVV( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg1_val, PINTOOL_REGISTER* r_reg2_val ){

    if (!GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    vector<string> conditional_expressions;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg1_val, ins_obj_ptr->read_vec_registers.front(), r_operands1, read_symbolic );
    get_string_values_reg_operand( r_reg2_val, ins_obj_ptr->read_vec_registers.back(), r_operands2, read_symbolic );
    
    if ( read_symbolic ){

        double value1;
        double value2;
        for ( uint32_t i = 0; i < r_operands2.size(); ++i ){
            value1 = get_double_value_reg( r_reg1_val, i, ins_obj_ptr->read_vec_registers.front()->n_bytes );
            value2 = get_double_value_reg( r_reg2_val, i, ins_obj_ptr->read_vec_registers.back()->n_bytes );
            conditional_expressions.push_back(get_relation(value1, value2, r_operands1[i], r_operands2[i]));
        }
    }

    if ( ins_obj_ptr->write_vec_registers.size() > 0 ){
        process_w_reg_operands(ins_obj_ptr, w_operands, r_operands1, conditional_expressions, read_symbolic);
    }
    else if ( read_symbolic ){
        add_smt_text(ins_obj_ptr, w_operands, r_operands1, conditional_expressions);
    }
}

void cmp_readVM( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg_val, ADDRINT r_base_address ){

    if (!GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    vector<string> conditional_expressions;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg_val, ins_obj_ptr->read_vec_registers.front(), r_operands1, read_symbolic );
    get_string_values_mem_operand( r_base_address, ins_obj_ptr->read_memory.back(), r_operands2, read_symbolic );
    
    if ( read_symbolic ){

        double value1;
        double value2;
        for ( uint32_t i = 0; i < r_operands2.size(); ++i ){
            value1 = get_double_value_reg( r_reg_val, i, ins_obj_ptr->read_vec_registers.back()->n_bytes );
            value2 = get_double_value_mem( r_base_address + i*ins_obj_ptr->read_memory.back()->n_bytes, ins_obj_ptr->read_memory.back()->n_bytes );
            conditional_expressions.push_back(get_relation(value1, value2, r_operands1[i], r_operands2[i]));
        }
    }

    if ( ins_obj_ptr->write_vec_registers.size() > 0 ){
        process_w_reg_operands(ins_obj_ptr, w_operands, r_operands1, conditional_expressions, read_symbolic);
    }
    else if ( read_symbolic ){
        add_smt_text(ins_obj_ptr, w_operands, r_operands1, conditional_expressions);
    }
}

void writeV_readV( ADDRINT rtn_id, ADDRINT ins_offset,  PINTOOL_REGISTER* r_reg_val ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg_val, ins_obj_ptr->read_vec_registers.back(), r_operands1, read_symbolic );
    process_w_reg_operands( ins_obj_ptr, w_operands, r_operands1, r_operands2, read_symbolic );
}

void writeV_readM( ADDRINT rtn_id, ADDRINT ins_offset,  ADDRINT r_base_address ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    bool read_symbolic = false;

    get_string_values_mem_operand( r_base_address, ins_obj_ptr->read_memory.back(), r_operands1, read_symbolic );
    process_w_reg_operands( ins_obj_ptr, w_operands, r_operands1, r_operands2, read_symbolic );
}

void writeM_readV( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT w_base_address,  PINTOOL_REGISTER* r_reg_val ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg_val, ins_obj_ptr->read_vec_registers.back(), r_operands1, read_symbolic );
    process_w_mem_operands( ins_obj_ptr, w_base_address, w_operands, r_operands1, r_operands2, read_symbolic );
}

void writeV_readVM( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg_val, ADDRINT r_base_address ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg_val, ins_obj_ptr->read_vec_registers.back(), r_operands1, read_symbolic );
    get_string_values_mem_operand( r_base_address, ins_obj_ptr->read_memory.back(), r_operands2, read_symbolic );
    process_w_reg_operands( ins_obj_ptr, w_operands, r_operands1, r_operands2, read_symbolic );
}

void writeV_readVV( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg1_val, PINTOOL_REGISTER* r_reg2_val ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    vector<string> r_operands1;
    vector<string> r_operands2;
    vector<string> w_operands;
    bool read_symbolic = false;

    get_string_values_reg_operand( r_reg1_val, ins_obj_ptr->read_vec_registers.front(), r_operands1, read_symbolic );
    get_string_values_reg_operand( r_reg2_val, ins_obj_ptr->read_vec_registers.back(), r_operands2, read_symbolic );

    process_w_reg_operands( ins_obj_ptr, w_operands, r_operands1, r_operands2, read_symbolic );
}

void process_w_reg_operands( Instruction* ins_obj_ptr, vector<string>& w_operands, vector<string>& r_operands1, vector<string>& r_operands2, bool& read_symbolic ){

    // create fresh symbolic variables for write operands, but only add them to ACTIVE_SYM_VAR_NAMES within the add_smt_text routine if certain conditions are met (instruction dependent)
    // in effect, this means the old symbolic variables for the write operands will be dead from the perspective of future reads
    string sym_var_uniq_name;
    string sym_var_base_name = reg_to_sym_var_base_name(ins_obj_ptr->write_vec_registers.back(), true);
    for ( uint32_t i = 0; i < REG_Size(ins_obj_ptr->write_vec_registers.back()->reg)/ins_obj_ptr->write_vec_registers.back()->n_bytes; ++i ){
        sym_var_uniq_name = sym_var_base_name + "_" + to_string(i);
        w_operands.push_back(sym_var_uniq_name);
    }
    if ( read_symbolic ){
        add_smt_text(ins_obj_ptr, w_operands, r_operands1, r_operands2);
    }
}

void process_w_mem_operands( Instruction* ins_obj_ptr, ADDRINT mem_base_address, vector<string>& w_operands, vector<string>& r_operands1, vector<string>& r_operands2, bool& read_symbolic ){

    // check if write addresses contain symbolic values
    get_string_values_mem_operand( mem_base_address, ins_obj_ptr->write_memory.back(), w_operands, read_symbolic );

    // create fresh symbolic variables for write operands, but only add them to ACTIVE_SYM_VAR_NAMES within the add_smt_text routine if certain conditions are met (instruction dependent)
    // in effect, this means the old symbolic variables for the write operands will be dead from the perspective of future reads
    string sym_var_uniq_name;
    for ( uint32_t i = 0; i < ins_obj_ptr->write_memory.back()->n_values; ++i ){
        ADDRINT effective_address = mem_base_address + i * ins_obj_ptr->write_memory.back()->n_bytes;
        sym_var_uniq_name = mem_to_sym_var_uniq_name(ins_obj_ptr->write_memory.back(), effective_address, true);
        w_operands[i] = sym_var_uniq_name;
    }
    if ( read_symbolic ){
        add_smt_text(ins_obj_ptr, w_operands, r_operands1, r_operands2);   
    }
}

void add_smt_text_helper( Instruction* ins_obj_ptr, string w_operand, string r_operand1, string r_operand2 ){

    // note that symbolic write memory operands will always be relevant, regardless of whether the read operands are symbolic
    bool symbolic_write = false;

    if ( w_operand[0] == 'm' ){
        symbolic_write = true;
    }
    else{
        if ( r_operand1 != "" && ((r_operand1[0] == 'm') || (r_operand1[0] == 'r')) ){
            symbolic_write = true;
        }
        if ( r_operand2 != "" && ((r_operand2[0] == 'm') || (r_operand2[0] == 'r')) ){
            symbolic_write = true;
        }
    }

    if ( symbolic_write == true ){
        ACTIVE_SYM_VAR_NAMES.insert(w_operand);
    }
}

void add_smt_text( Instruction* ins_obj_ptr, const vector<string>& w_operands, const vector<string>& r_operands1, const vector<string>& r_operands2 ){
    // here, the rightmost read operands are used for non bitvector ops to determine n_values as VEX instructions can do some
    // screwy things that cause the first read operand to have more reads than expected (copying over values before zeroing them out)
    // or that cause the write operand to have more writes that expected (zeroing out upper bits)

    
    string smtlib2_op = ins_obj_ptr->smtlib2_op;
    uint32_t n_written_values;
    string null_arg = "";
    if ( smtlib2_op == "$bvxor" ){
        assert( r_operands1.size() == r_operands2.size() );
        n_written_values = r_operands2.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( r_operands1[i] == BV32_NEG || r_operands1[i] == BV64_NEG ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_NEG || r_operands2[i] == BV64_NEG ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else if ( r_operands1[i] == BV32_ZERO || r_operands1[i] == BV64_ZERO ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_ZERO || r_operands2[i] == BV64_ZERO ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else if ( r_operands1[i] == r_operands2[i] ){
                continue;
            }
            else{
                assert(false && "XOR op without a BV_ZERO, BV_NEG, or identical operands");
            }
        }
    }
    else if ( smtlib2_op == "$bvand" ){
        assert( r_operands1.size() == r_operands2.size() );
        n_written_values = r_operands2.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( r_operands1[i] == BV32_ABS || r_operands1[i] == BV64_ABS ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_ABS || r_operands2[i] == BV64_ABS ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else if ( r_operands1[i] == BV32_ID || r_operands1[i] == BV64_ID ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_ID || r_operands2[i] == BV64_ID ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else if ( r_operands1[i] == BV32_ZERO || r_operands1[i] == BV64_ZERO || r_operands2[i] == BV32_ZERO || r_operands2[i] == BV64_ZERO ){
                continue;
            }
            else{
                assert(false && "AND op without a BV_ABS, BV_ID, or BV_ZERO operand");
            }
        }
    }
    else if ( smtlib2_op == "$bvor" ){
        assert( r_operands1.size() == r_operands2.size() );
        n_written_values = r_operands2.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( r_operands1[i] == BV32_ZERO || r_operands1[i] == BV64_ZERO ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_ZERO || r_operands2[i] == BV64_ZERO ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else if ( r_operands1[i] == BV32_NEG || r_operands1[i] == BV64_NEG ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else if ( r_operands2[i] == BV32_NEG || r_operands2[i] == BV64_NEG ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else{
                assert(false && "OR op with unknown arithmetic purpose (i.e., not identity or negation)");
            }
        }
    }
    else if ( smtlib2_op == "$bvandn" ){
        assert( r_operands1.size() == r_operands2.size() );
        n_written_values = r_operands2.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( r_operands1[i] == BV32_ID || r_operands1[i] == BV64_ID ){
                continue;
            }
            else if ( r_operands2[i] == BV32_ZERO || r_operands2[i] == BV64_ZERO ){
                continue;
            }
            else if ( r_operands1[i] == BV32_ZERO || r_operands1[i] == BV64_ZERO ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            else{
                assert(false && "ANDN op with unknown arithmetic purpose (i.e., not identity or zeroing)");
            }
        }
    }
    else if ( smtlib2_op == "$cvt" ){

        // cvtsd2ss (only SSE encoding)
        // cvtss2sd (only SSE encoding)
        // cvtps2pd
        // cvtpd2ps
        if ( r_operands2.size() == 0 ){
            n_written_values = ins_obj_ptr->read_operands.back()->n_values;
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                if ( ins_obj_ptr->write_operands.back()->n_bytes == 4 ){
                }
                else if ( ins_obj_ptr->write_operands.back()->n_bytes == 8 ){
                }
                else{
                    assert(false && "$cvt op with unexpected write operand precision");
                }
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }

            // cvtpd2ps: both the SSE and VEX versions zero out the second quadword which is exceptional wrt other SSE intructions
            // setting n_written_values to 4 means the block of code at the end of add_smt_text that updates "untouched" write operand values
            // will ignore the second quadword in the case of SSE which will then leave the rest of the bits unmodified
            if ( (ins_obj_ptr->write_operands.back()->n_bytes == 4) && (n_written_values == 2) ){
                n_written_values = 4;
            }
        }

        // cvtsd2ss (only VEX encoding)
        // cvtss2sd (only VEX encoding)
        else{
            n_written_values = ins_obj_ptr->read_operands.back()->n_values;
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                if ( ins_obj_ptr->write_operands.back()->n_bytes == 4 ){
                }
                else if ( ins_obj_ptr->write_operands.back()->n_bytes == 8 ){
                }
                else{
                    assert(false && "$cvt op with unexpected write operand precision");
                }
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "$shufp" ){
        n_written_values = r_operands2.size();
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        string selected_arg = "";
        for ( uint32_t i = 0; i < n_written_values / 2; ++i ){
            if ( i%2 == 0 ){
                selected_arg = select(r_operands1, (i/2)*4, control, (i%2)*4);
            }
            else{
                selected_arg = select(r_operands2, (i/2)*4, control, (i%2)*4);
            }
            add_smt_text_helper(ins_obj_ptr, w_operands[2*i], selected_arg, null_arg);

            if ( i%2 == 0 ){
                selected_arg = select(r_operands1, (i/2)*4, control, ((i%2)*4)+2);
            }
            else{
                selected_arg = select(r_operands2, (i/2)*4, control, ((i%2)*4)+2);
            }
            add_smt_text_helper(ins_obj_ptr, w_operands[(2*i)+1], selected_arg, null_arg);
        }
    }
    else if ( smtlib2_op == "$pshuf" ){
        assert( r_operands2.size() == 0 );
        n_written_values = r_operands1.size();
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        string selected_arg = "";
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            selected_arg = select(r_operands1, (i/4)*4, control, i%4);
            add_smt_text_helper(ins_obj_ptr, w_operands[i], selected_arg, null_arg);
        }
    }
    else if ( smtlib2_op == "$permil" ){
        n_written_values = r_operands1.size();
        if ( ins_obj_ptr->immediates.size() > 0 ){
            bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
            string selected_arg = "";
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                selected_arg = select(r_operands1, (i/4)*4, control, i%4);
                add_smt_text_helper(ins_obj_ptr, w_operands[i], selected_arg, null_arg);
            }
        }
    }
    else if ( smtlib2_op == "$blend" ){
        n_written_values = r_operands2.size();
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( !control.test(i) ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            else{
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "$vinsertf128" ){
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        if ( control.test(0) ){
            for ( uint32_t i = 0; i < w_operands.size()/2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            for ( uint32_t i = 0; i < w_operands.size()/2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[w_operands.size()/2 + i], null_arg, r_operands2[i]);
            }
        }   
        else if ( !control.test(0) ){
            for ( uint32_t i = 0; i < w_operands.size()/2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
            for ( uint32_t i = w_operands.size()/2; i < w_operands.size(); ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
        }
        else{
            assert(false);
        }
    }
    else if ( smtlib2_op == "$vextractf128" ){
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        if ( control.test(0) ){
            for ( uint32_t i = 0; i < w_operands.size(); ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[r_operands1.size()/2 + i], null_arg);
            }
        }   
        else if ( !control.test(0) ){
            for ( uint32_t i = 0; i < w_operands.size(); ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
        }
        else{
            assert(false);
        }
    }
    else if ( smtlib2_op == "$unpckhp" ){
        n_written_values = r_operands2.size();
        string selected_arg = "";
        for ( uint32_t i = 0; i < r_operands2.size() / 2; ++i ){
            selected_arg = r_operands1[(8/ins_obj_ptr->write_operands.back()->n_bytes)+((i/2)*2)+i];
            add_smt_text_helper(ins_obj_ptr, w_operands[2*i], selected_arg, null_arg);

            selected_arg = r_operands2[(8/ins_obj_ptr->write_operands.back()->n_bytes)+((i/2)*2)+i];
            add_smt_text_helper(ins_obj_ptr, w_operands[(2*i) + 1], selected_arg, null_arg);
        }
    }
    else if ( smtlib2_op == "$unpcklp" ){
        n_written_values = r_operands2.size();
        string selected_arg = "";
        for ( uint32_t i = 0; i < r_operands2.size() / 2; ++i ){
            selected_arg = r_operands1[((i/2)*2)+i];
            add_smt_text_helper(ins_obj_ptr, w_operands[2*i], selected_arg, null_arg);

            selected_arg = r_operands2[((i/2)*2)+i];
            add_smt_text_helper(ins_obj_ptr, w_operands[(2*i)+1], selected_arg, null_arg);
        }
    }
    else if ( smtlib2_op == "$movhlps" ){
        if ( r_operands2.size() == 0 ){
            n_written_values = 2;
            for ( uint32_t i = 0; i < 2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[2+i], null_arg);
            }
        }
        else{
            n_written_values = 4;
            for ( uint32_t i = 0; i < 2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[2+i]);

                add_smt_text_helper(ins_obj_ptr, w_operands[2+i], r_operands1[2+i], null_arg);
            }
        }
    }
    else if ( smtlib2_op == "$movlhps" ){
        n_written_values = 4;
        if ( r_operands2.size() == 0 ){
            for ( uint32_t i = 0; i < 2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[2+i], r_operands1[i], null_arg);
            }
            // if either of the first two write operands are a symbolic register (and not a bitvector literal), 
            // add an assertion that the new symbolic write variable is equivalent to the old symbolic write variable
            for ( uint32_t i = 0; i < 2; ++i ){                
                if ( w_operands[i][0] == 'r' ){ // sym var names are of the form reg_xmm0_i_j
                    string previous_sym_var_name = w_operands[i];
                    size_t second_underscore_pos = previous_sym_var_name.find('_', 7); // len(reg_xmm) = 7
                    size_t third_underscore_pos = previous_sym_var_name.find('_', second_underscore_pos);
                    string previous_integer = to_string(stoi(previous_sym_var_name.substr(second_underscore_pos + 1, third_underscore_pos - second_underscore_pos - 1)) - 1);
                    previous_sym_var_name.replace(second_underscore_pos + 1, third_underscore_pos - second_underscore_pos - 1, previous_integer);
                    if ( ACTIVE_SYM_VAR_NAMES.find(previous_sym_var_name) != ACTIVE_SYM_VAR_NAMES.end() ){
                        add_smt_text_helper(ins_obj_ptr, w_operands[i], previous_sym_var_name, null_arg);
                    }
                }
            }
        }
        else{
            for ( uint32_t i = 0; i < 2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);

                add_smt_text_helper(ins_obj_ptr, w_operands[2+i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "$movhp" ){
        if ( r_operands2.size() == 0 ){
            n_written_values = 8 / ins_obj_ptr->write_operands.back()->n_bytes;
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[n_written_values + i], r_operands1[i], null_arg);
            }
        }
        else{
            n_written_values = 16 / ins_obj_ptr->write_operands.back()->n_bytes;
            for ( uint32_t i = 0; i < n_written_values/2; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
            for ( uint32_t i = n_written_values; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[n_written_values + i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "$movlp" ){
        n_written_values = 8 / ins_obj_ptr->write_operands.back()->n_bytes;
        if ( r_operands2.size() == 0 ){
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }
        }
        else{
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "$movevendup" ){
        n_written_values = r_operands1.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[2*(i/2)], null_arg);
        }
    }
    else if ( smtlib2_op == "$movshdup" ){
        n_written_values = r_operands1.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[2*(i/2)+1], null_arg);
        }
    }
    else if ( smtlib2_op == "$insert" ){
        n_written_values = r_operands2.size();
        bitset<64> control(ins_obj_ptr->immediates.back()->imm_value);
        string tmp;
        vector<string> tmp2;

        if ( ins_obj_ptr->read_memory.size() > 0 ){
            tmp = r_operands2[0];
        }
        else if ( !control.test(7) && !control.test(6) ){
            tmp = r_operands2[0];
        }
        else if ( !control.test(7) && control.test(6) ){
            tmp = r_operands2[1];
        }
        else if ( control.test(7) && !control.test(6) ){
            tmp = r_operands2[2];
        }
        else if ( control.test(7) && control.test(6) ){
            tmp = r_operands2[3];
        }

        if ( !control.test(5) && !control.test(4) ){
            tmp2.push_back(tmp);
            tmp2.push_back(r_operands1[1]);
            tmp2.push_back(r_operands1[2]);
            tmp2.push_back(r_operands1[3]);
        }
        else if ( !control.test(5) && control.test(4) ){
            tmp2.push_back(r_operands1[0]);
            tmp2.push_back(tmp);
            tmp2.push_back(r_operands1[2]);
            tmp2.push_back(r_operands1[3]);
        }
        else if ( control.test(5) && !control.test(4) ){
            tmp2.push_back(r_operands1[0]);
            tmp2.push_back(r_operands1[1]);
            tmp2.push_back(tmp);
            tmp2.push_back(r_operands1[3]);
        }
        else if ( control.test(5) && control.test(4) ){
            tmp2.push_back(r_operands1[0]);
            tmp2.push_back(r_operands1[1]);
            tmp2.push_back(r_operands1[2]);
            tmp2.push_back(tmp);
        }

        for ( uint32_t i = 0; i < 4; ++i ){
            if ( !control.test(i) ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], tmp2[i], null_arg);
            }
        }
    }
    else if ( smtlib2_op == "$mov" ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        if ( r_operands2.size() == 0 ){
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
            }

            // if second quadword is not written, this will effectively ignore it for reg writes
            if ( r_operands1.size() < 128/(ins_obj_ptr->read_operands.back()->n_bytes*8) ){
                n_written_values = 128/(ins_obj_ptr->read_operands.back()->n_bytes*8);
            }
        }
        else{
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
            }
        }
    }
    else if ( smtlib2_op == "fp.sqrt" ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], null_arg, r_operands2[i]);
        }
    }
    else if ( smtlib2_op == "$cmp" ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        // in this case, r_operands2 contains the full conditional expression  
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( r_operands2[i] == "nan_comparison" ){
                log2("comparison with NaN; no constraint added");
            }
        }
    }
    else if ( smtlib2_op == "$vbroadcasts" ){
        n_written_values = w_operands.size();
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[0], null_arg);
        }
    }
    else if ( (smtlib2_op == "fp.max") || (smtlib2_op == "fp.min") ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], r_operands2[i]);
        }
    }
    else if ( smtlib2_op == "$rcp" ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            if ( ins_obj_ptr->write_operands.back()->n_bytes == 4 ){
            }
            else if ( ins_obj_ptr->write_operands.back()->n_bytes == 8 ){
            }
            else{
                assert(false && "$rcp op with unexpected write operand precision");
            }
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
        }
    }
    else if ( smtlib2_op == "$hadd" ){
        n_written_values = ins_obj_ptr->read_operands.back()->n_values / 2;
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[2*i], r_operands1[2*i+1]);
        }
        if ( r_operands2.size() > 0 ){
            for ( uint32_t i = 0; i < n_written_values; ++i ){
                add_smt_text_helper(ins_obj_ptr, w_operands[n_written_values + i], r_operands2[2*i], r_operands2[2*i+1]);
            }
        }
    }
    else{
        // fp.add, fp.sub, fp.mul, fp.div...
        n_written_values = ins_obj_ptr->read_operands.back()->n_values;
        for ( uint32_t i = 0; i < n_written_values; ++i ){
            add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], r_operands2[i]);
        }
    }

    // update "untouched" write operand values

    // scalar vex-encoded instructions copy bits 128:[64,32] from the first read operand to the write operand if the write operand is a register
    if ( ins_obj_ptr->disassembly[0] == 'v' ){
        if ( (n_written_values == 1) && (w_operands.size() > 1) ){
            for ( uint32_t i = 1; i < 128 / (ins_obj_ptr->read_operands.front()->n_bytes*8); ++i ){

                // but we only need to do this if the value is symbolic
                if ( r_operands1[i][0] == 'r' ){
                    add_smt_text_helper(ins_obj_ptr, w_operands[i], r_operands1[i], null_arg);
                }
            }
        }
    }
    // both scalar and packed legacy sse encoded instructions leave the unwritten bits of the write operand unmodified...
    else{
        for ( uint32_t i = n_written_values; i < w_operands.size(); ++i ){
            
            // ...so, if this write operand is a symbolic register (and not a bitvector literal), 
            // add an assertion that the new symbolic write variable is equivalent to the old symbolic write variable
            if ( w_operands[i][0] == 'r' ){ // sym var names are of the form reg_xmm0_i_j
                string previous_sym_var_name = w_operands[i];
                size_t second_underscore_pos = previous_sym_var_name.find('_', 7); // len(reg_xmm) = 7
                size_t third_underscore_pos = previous_sym_var_name.find('_', second_underscore_pos);
                string previous_integer = to_string(stoi(previous_sym_var_name.substr(second_underscore_pos + 1, third_underscore_pos - second_underscore_pos - 1)) - 1);
                previous_sym_var_name.replace(second_underscore_pos + 1, third_underscore_pos - second_underscore_pos - 1, previous_integer);
                if ( ACTIVE_SYM_VAR_NAMES.find(previous_sym_var_name) != ACTIVE_SYM_VAR_NAMES.end() ){
                    add_smt_text_helper(ins_obj_ptr, w_operands[i], previous_sym_var_name, null_arg);
                }
            }
        }
    }
}


string get_bitvector_string_reg( PINTOOL_REGISTER* reg, uint32_t value_idx, uint32_t n_bytes ){
    string value;
    if ( n_bytes == 8 ){
        union {
            double in;
            uint64_t out;
        } data;
        data.in = reg->dbl[value_idx];
        bitset<64> bits(data.out);
        value = "( ( _ to_fp 11 53 ) #b";
        for ( int32_t i = 63; i >= 0; --i ){
            value = value + to_string(bits[i]);
        }
    }
    else if ( n_bytes == 4 ){
        union {
            float in;
            uint32_t out;
        } data;
        data.in = reg->flt[value_idx];
        bitset<32> bits(data.out);
        value = "( ( _ to_fp 8 24 ) #b";
        for ( int32_t i = 31; i >= 0; --i ){
            value = value + to_string(bits[i]);
        }
    }
    value = value + " )";
    return value;
}

string get_bitvector_string_mem( ADDRINT address, uint32_t n_bytes ){
    string value;
    if ( n_bytes == 8 ){
        union {
            double in;
            uint64_t out;
        } data;
        data.in = *(reinterpret_cast<double*>(address));
        bitset<64> bits(data.out);
        value = "( ( _ to_fp 11 53 ) #b";
        for ( int32_t i = 63; i >= 0; --i ){
            value = value + to_string(bits[i]);
        }
    }
    else if ( n_bytes == 4 ){
        union {
            float in;
            uint32_t out;
        } data;
        data.in = *(reinterpret_cast<float*>(address));
        bitset<32> bits(data.out);
        value = "( ( _ to_fp 8 24 ) #b";
        for ( int32_t i = 31; i >= 0; --i ){
            value = value + to_string(bits[i]);
        }
    }
    value = value + " )";
    return value;
}

string select( vector<string> src, uint32_t src_start_idx, bitset<64> ctrl, uint32_t ctrl_start_idx ){
    if ( !ctrl.test(ctrl_start_idx+1) && !ctrl.test(ctrl_start_idx) ){
        return src[src_start_idx];
    }
    else if ( !ctrl.test(ctrl_start_idx+1) && ctrl.test(ctrl_start_idx) ){
        return src[src_start_idx+1];
    }
    else if ( ctrl.test(ctrl_start_idx+1) && !ctrl.test(ctrl_start_idx) ){
        return src[src_start_idx+2];
    }
    else{
        return src[src_start_idx+3];
    }   
}

template <typename T>
vector<ADDRINT> check_for_EVs_mem( ADDRINT base_address, uint32_t n_values, uint32_t n_bytes, bool print ){

    vector<ADDRINT> EV_addresses;

    for ( uint32_t i=0; i < n_values; ++i){
        T value = *(reinterpret_cast<T*>(base_address + i * (n_bytes)));

        if ( print ){
            WRITE_BUFFER << value << " ";
        }

        if ( isnan(value) || isinf(value) ){
            EV_addresses.push_back((ADDRINT)(base_address + i * (n_bytes)));
        }
    }

    return EV_addresses;
}

template <typename T>
void post_call( T* return_value_ptr ){
    
    if ( MODE == EV_OVERWRITE && EV_GENERATOR_HASH == 0 ){
        N_MISSES++;
        return;
    }

    if ( SYS_WRITE_INTERCEPT_FLAG == true ){
        SYS_WRITE_INTERCEPT_FLAG = false;
        WRITE_BUFFER << "\n================== END STDOUT / STDERR ==================\n";
    }

    // check the output vars
    int32_t injected_io_var_ptr_idx = -1;
    for ( const auto& var_idx : TARGETED_FUNC_STATIC_INFO->process_order ){
        ++injected_io_var_ptr_idx;

        VarProperties var_properties = TARGETED_FUNC_STATIC_INFO->io_vars[var_idx];

        if ( var_properties.type == REAL_OUT || var_properties.type == REAL_INOUT ){
            uint32_t n_values = TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx];

            WRITE_BUFFER << "(out) " << var_properties.name << ": ";

            vector<ADDRINT> EV_addresses;
            if ( var_properties.n_bytes == 4 ){
                EV_addresses = check_for_EVs_mem<float>(INJECTED_IO_VAR_PTRS[injected_io_var_ptr_idx], n_values, 4, true);
            }
            else if ( var_properties.n_bytes == 8 ){
                EV_addresses = check_for_EVs_mem<double>(INJECTED_IO_VAR_PTRS[injected_io_var_ptr_idx], n_values, 8, true);
            }
            else{
                assert(false && "float types other than 32-bit and 64-bit not yet supported");
            }

            if ( EV_addresses.size() > 0 ){
                EXIT_CODE = max(1, EXIT_CODE);
            }
            WRITE_BUFFER << endl;
        }
        else if ( var_properties.type == REAL_RETURN ){
            assert( return_value_ptr != NULL );

            WRITE_BUFFER << "(out) " << var_properties.name << ": " << *return_value_ptr << endl;

            if ( isnan(*return_value_ptr) || isinf(*return_value_ptr) ){
                EXIT_CODE = max(1, EXIT_CODE);
            }
        }
    }

    if ( MODE == BASELINE_ONLY ){
        string out_file_path = "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/" + TARGETED_FUNC_STATIC_INFO->name + "." + to_string(CURRENT_IO_VARS->first) + ".out";
        ofstream out(out_file_path.c_str());
        out << WRITE_BUFFER.str() << endl;
    }
    else if ( MODE == PRE_EV_OVERWRITE_BASELINE ){
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_sec = end.tv_sec - START_TIME.tv_sec;
        double elapsed_nsec = end.tv_nsec - START_TIME.tv_nsec;
        TIME_LIMIT = 10 * (elapsed_sec + elapsed_nsec / 1e9);
    }
    else if ( MODE == EV_OVERWRITE ) {
        if (EXIT_CODE == 0 || EXIT_CODE == 124){
            N_EXIT0++;
            string out_file_path = "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/" + TARGETED_FUNC_STATIC_INFO->name + "." + to_string(CURRENT_IO_VARS->first) + "." + to_string(EV_GENERATOR_HASH) + ".out";
            ofstream out(out_file_path.c_str());
            out << WRITE_BUFFER.str() << endl;
        }
        else{
            N_EXIT1++;
        }
    }
}

void inject_io_var( ADDRINT* actual_val_ptr, ADDRINT var_idx ){

    // don't inject any vars for recursive calls
    if ( CALL_STACK.size() > 0 ){
        return;
    }

    TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx] = get_n_values(var_idx, TARGETED_FUNC_DYNAMIC_INFO, TARGETED_FUNC_STATIC_INFO);
    uint32_t total_bytes = TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes * TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx];

    // construct replacement arg in the target program's address space
    for ( uint32_t i=0; i < total_bytes; ++i ){
        uint8_t* temp = IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET;
        *temp = *CURRENT_IO_VARS_BYTES_IT;
        ++CURRENT_IO_VARS_BYTES_IT;
        ++IO_VAR_BUFFER_OFFSET;
    }

    uint64_t base_address = (uint64_t)IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes;
    uint32_t offset = 0;
    for ( uint32_t i=0; i < TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx]; ++i ){

        if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type >= REAL_IN ){
            string sym_var_name = "mem_" + hexstr(base_address + offset);
            SYM_VAR_COUNTERS[sym_var_name] = 0;
            sym_var_name = sym_var_name + "_" + to_string(SYM_VAR_COUNTERS[sym_var_name]) + "_b" + to_string(TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes * 8);
            string sym_input_var_name = TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].name + "__" + to_string(i) + "__";
            ACTIVE_SYM_VAR_NAMES.insert(sym_var_name);
            ACTIVE_SYM_VAR_NAMES.insert(sym_input_var_name);
            log2("init symbolic " + sym_var_name + " for " + sym_input_var_name);
        }
        offset = offset + TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes;
    }

    // replace the old val ptr and save the new ptr
    INJECTED_IO_VAR_PTRS.push_back((ADDRINT) IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes);
    *actual_val_ptr = INJECTED_IO_VAR_PTRS.back();

    // maintain alignment
    IO_VAR_BUFFER_OFFSET = IO_VAR_BUFFER_OFFSET + max(0, 4 - (int)total_bytes);

    // skip extra space
    IO_VAR_BUFFER_OFFSET = IO_VAR_BUFFER_OFFSET + IO_VAR_EXTRA_SPACE;

    // save value for use in evaluating the n_value_strings of other io_vars later in the determined process_order
    if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == INTEGER ){
        TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_value[var_idx] = *(reinterpret_cast<int*>(*actual_val_ptr));
    }
    else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == CHARACTER ){
        TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_value[var_idx] = (int)(*(reinterpret_cast<char*>(*actual_val_ptr)));
    }
    else{
        TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_value[var_idx] = 0;
    }

    WRITE_BUFFER << "(in) " << TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].name << ": ";
    for ( uint32_t i=0; i < TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx]; ++i ){
        if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == INTEGER ){
            WRITE_BUFFER << (*(reinterpret_cast<int**>(actual_val_ptr)))[i] << " ";
        }
        else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == CHARACTER ){
            WRITE_BUFFER << (*(reinterpret_cast<char**>(actual_val_ptr)))[i] << " ";
        }
        else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type >= REAL_IN ){

#ifdef EVS_IN_INPUTS
            if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == REAL_IN || TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == REAL_INOUT ){
                vector<string> EV_generator_id;
                EV_generator_id.push_back(TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].name);
                EV_generator_id.push_back(to_string(i));
                size_t temp = EV_GENERATOR_HASH_FUNCTION(EV_generator_id);

                if ( MODE == PRE_EV_OVERWRITE_BASELINE ){
                    IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].insert(temp);
                }
                else if ( EV_GENERATOR_HASH == 0 ){
                    auto it = IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].find(temp);
                    if ( it != IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].end() ) {
                        N_HITS++;

                        IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].erase(it);

                        EV_GENERATOR_HASH = temp;
                        if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 4 ){
                            (*(reinterpret_cast<float**>(actual_val_ptr)))[i] = 0.0/0.0;
                        }
                        else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 8 ){
                            (*(reinterpret_cast<double**>(actual_val_ptr)))[i] = 0.0/0.0;
                        }
                        else{
                            assert(false && "unsupported type for EV overwriting");
                        }
                    }
                }
            }
#endif

            if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 4 ){
                WRITE_BUFFER << (*(reinterpret_cast<float**>(actual_val_ptr)))[i] << " ";
            }
            else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 8 ){
                WRITE_BUFFER << (*(reinterpret_cast<double**>(actual_val_ptr)))[i] << " ";
            }
        }
    }
    WRITE_BUFFER << endl;
}

void pre_call( ADDRINT rtn_id ){
    CALL_STACK.push_back(rtn_id);
}

void save_return_context( const CONTEXT* ctxt ){
    if ( MODE == PRE_EV_OVERWRITE_BASELINE ){
        delete RETURN_CONTEXT;
        RETURN_CONTEXT = new CONTEXT;
        PIN_SaveContext(ctxt, RETURN_CONTEXT);
    }
}

void reset_and_run( CONTEXT* ctxt, const pair<const size_t,vector<uint8_t>>* io_vars ){
    CURRENT_IO_VARS=io_vars;
    CURRENT_IO_VARS_BYTES_IT = CURRENT_IO_VARS->second.begin();

    // construct MXCSR contents in the target program's address space
    IO_VAR_BUFFER_OFFSET = 0;
    for ( uint32_t i=0; i < 2; ++i ){
        uint8_t* temp = IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET;
        *temp = *CURRENT_IO_VARS_BYTES_IT;
        ++CURRENT_IO_VARS_BYTES_IT;
        ++IO_VAR_BUFFER_OFFSET;
    }
    ADDRINT mxcsr_contents = (ADDRINT) *(reinterpret_cast<uint16_t*>(IO_VAR_BUFFER_BASE_PTR));
    PIN_SetContextReg(ctxt, REG_MXCSR, mxcsr_contents);
    IO_VAR_BUFFER_OFFSET = 0; // now that MXCSR reg has been set, we reset offset so buffer will be filled with actual io vars

    CALL_STACK.clear();
    WRITE_BUFFER.str("");
    EXIT_CODE = 0;
    EV_GENERATOR_HASH = 0;
    INSTRUCTION_EXECUTION_COUNTS.clear();
    SYM_VAR_COUNTERS.clear();
    ACTIVE_SYM_VAR_NAMES.clear();
    INJECTED_IO_VAR_PTRS.clear();
    clock_gettime(CLOCK_MONOTONIC, &START_TIME);
    GO = true;

    for ( uint32_t var_idx=0; var_idx < TARGETED_FUNC_STATIC_INFO->io_vars.size(); ++var_idx ){
        VarProperties var_properties = TARGETED_FUNC_STATIC_INFO->io_vars[var_idx];
        if ( var_properties.type == REAL_RETURN ){
            if ( var_properties.n_bytes == 4 ){
                float res;
                PIN_CallApplicationFunction(ctxt, PIN_ThreadId(), CALLINGSTD_DEFAULT, (AFUNPTR) TARGETED_FUNCTION_PTR, NULL, PIN_PARG(float), &res, PIN_PARG_END());
                post_call<float>(&res);
                return;
            }
            else if ( var_properties.n_bytes == 8 ){
                double res;
                PIN_CallApplicationFunction(ctxt, PIN_ThreadId(), CALLINGSTD_DEFAULT, (AFUNPTR) TARGETED_FUNCTION_PTR, NULL, PIN_PARG(double), &res, PIN_PARG_END());
                post_call<double>(&res);
                return;
            }
            else{
                assert(false && "float types other than 32-bit and 64-bit not yet supported");
            }
        }
    }

    PIN_CallApplicationFunction(ctxt, PIN_ThreadId(), CALLINGSTD_DEFAULT, (AFUNPTR) TARGETED_FUNCTION_PTR, NULL, PIN_PARG_END());
    post_call<double>(NULL);
    return;
}

void perform_analysis_loop( CONTEXT *ctxt ){

    // maintain alignment
    // and add space between each io_var
    uint32_t io_var_buffer_size = MAX_IO_VAR_BYTES;
    for ( const auto& i : TARGETED_FUNC_STATIC_INFO->process_order ){
        if ( TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes % 4 != 0 ){
            io_var_buffer_size = io_var_buffer_size + max(0, 4 - (int)TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes);
        }
        io_var_buffer_size = io_var_buffer_size + IO_VAR_EXTRA_SPACE;
    }
    IO_VAR_BUFFER_BASE_PTR = (uint8_t*) malloc((io_var_buffer_size) * sizeof(uint8_t));

    uint32_t total_overwrite_count = 0;
    for ( const auto& x : SAVED_IO_VARS_MAP ){

        log1("===================== " + to_string(x.first) + " =====================");

        // if ( to_string(x.first) != "148063107919244560" ){
        //     continue;
        // }

        // if ( SKIP.find(to_string(x.first)) != SKIP.end() ){
        //     continue;
        // }
        // else{
        //     ofstream outfile("__temp");
        //     outfile << to_string(x.first) << endl;
        //     outfile.close();
        // }
        
        // baseline run
        reset_and_run(ctxt, &x);

        if ( MODE == PRE_EV_OVERWRITE_BASELINE ){
            MODE = EV_OVERWRITE;
            total_overwrite_count = total_overwrite_count + IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[x.first].size();
            uint32_t max_iters = 1.5 * IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[x.first].size();
            uint32_t i = -1;
            while ( ++i < max_iters && IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[x.first].size() > 0 ){
                reset_and_run(ctxt, &x);
            }
            MODE = PRE_EV_OVERWRITE_BASELINE;
        }
    }

    if ( MODE > BASELINE_ONLY ){
        cout << "               Spoofed Exceptions: " << N_HITS << endl; 
        // cout << "         Total Possible: " << total_overwrite_count << endl;
        // cout << "                         -> " << fixed << setprecision(4) << 100 * N_HITS / (total_overwrite_count) << "%" << endl;
        cout << "      Exception-Handling Failures: " << N_EXIT0 << endl;
        cout << "                     Failure Rate: " << fixed << setprecision(4) << 100 * (double) N_EXIT0 / (double) N_HITS << "%" << endl;
        // cout << "           EXIT=0 Count: " << N_EXIT0 << endl;
        cout << "                       Miss Count: " << N_MISSES << endl;
    }

    free(IO_VAR_BUFFER_BASE_PTR);
    delete TARGETED_FUNC_DYNAMIC_INFO;
    delete TARGETED_FUNC_STATIC_INFO;
    delete RETURN_CONTEXT;
    for ( const auto& x : INSTRUCTION_OBJ_MAP ){
        for ( const auto& y : x.second ){
            delete y.second;
        }
    }
    exit(0);
}

void image_pass2( IMG img, void *v ){
    RTN rtn = RTN_FindByName(img, "main");
    if ( RTN_Valid(rtn) ){
        RTN_Open(rtn);
        RTN_InsertCall( rtn, IPOINT_BEFORE, (AFUNPTR) perform_analysis_loop, IARG_CONTEXT, IARG_END );
        RTN_Close(rtn);
    }
}

void error_handler_called(){
    if ( GO && MODE == EV_OVERWRITE ){
        EXIT_CODE = max(1, EXIT_CODE);
        WRITE_BUFFER << "\n**EXCVATE: ERROR HANDLER CALLED\n\n";
        log2("ERROR HANDLER CALLED");
    }
}

void dump_inputs(){
    log1(WRITE_BUFFER.str());
}

void image_pass1( IMG img, void *v ){
    for ( auto& error_handler_name : ERROR_HANDLER_NAMES ){
        RTN rtn = RTN_FindByName(img, error_handler_name.c_str());
        if ( RTN_Valid(rtn) ){
            RTN_Open(rtn);
            RTN_InsertCall(
                rtn,
                IPOINT_BEFORE,
                (AFUNPTR) error_handler_called,
                IARG_END
            );
            RTN_Close(rtn);
        }
    }

    // search for targeted function
    RTN rtn = RTN_FindByName(img, TARGETED_FUNC_STATIC_INFO->name.c_str());
    if ( RTN_Valid(rtn) ){
        RTN_Open(rtn);

        TARGETED_FUNCTION_PTR = RTN_Funptr(rtn);

        // insert functions to process each io var at runtime
        for ( const auto& i : TARGETED_FUNC_STATIC_INFO->process_order ){
            if ( TARGETED_FUNC_STATIC_INFO->io_vars[i].type != REAL_RETURN ){
                RTN_InsertCall(
                    rtn,
                    IPOINT_BEFORE,
                    (AFUNPTR) inject_io_var,
                    IARG_FUNCARG_ENTRYPOINT_REFERENCE, i,
                    IARG_ADDRINT, i,
                    IARG_END
                );
            }
        }
#if VERBOSE >= 1
        RTN_InsertCall(
            rtn,
            IPOINT_BEFORE,
            (AFUNPTR) dump_inputs,
            IARG_END
        );
#endif
        RTN_Close(rtn);
    }

    // iterate through sections in this image
    for ( SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec) ){

        // if this section is executable, instrument instructions of the routines contained within
        if ( SEC_IsExecutable(sec) ){
            for ( RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn) ){
                RTN_Open(rtn);

                ROUTINE_NAME_MAP[RTN_Id(rtn)] = RTN_Name(rtn);

                RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR) pre_call, IARG_ADDRINT, RTN_Id(rtn), IARG_END);
                RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR) save_return_context, IARG_CONST_CONTEXT, IARG_END);

                // if ( VERBOSE >= 1 ){
                    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
                        instrument_instruction(ins);
                    }
                // }
                // else{
                //     for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
                //         if ( is_EV_generator_reg(INS_Opcode(ins)) ){
                //             instrument_instruction(ins);
                //         }
                //     }
                // }

                RTN_Close(rtn);
            }
        }
    }
}

void print_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode ){
    if ( GO ){
        string containing_rtn_name = ROUTINE_NAME_MAP[rtn_id];
        ostringstream oss;
        oss << setw(30) << left << containing_rtn_name + "::" + hexstr(ins_offset) << setw(6) << opcode << setw(50) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->disassembly << setw(100) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->src_location;
        log1(oss.str());
    }
}

void log_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode ){
    if ( GO ){
        string containing_rtn_name = ROUTINE_NAME_MAP[rtn_id];
        ostringstream oss;
        oss << setw(30) << left << containing_rtn_name + "::" + hexstr(ins_offset) << setw(6) << opcode << setw(50) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->disassembly << setw(100) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->src_location;
        log0(oss.str());
    }
}

void get_string_values_reg_operand( PINTOOL_REGISTER* reg_val, Operand* reg_obj, vector<string>& operand_values, bool& read_symbolic ){
    string sym_var_uniq_name = "";
    string sym_var_base_name = reg_to_sym_var_base_name(reg_obj, false);
    for ( uint32_t i = 0; i < REG_Size(reg_obj->reg)/reg_obj->n_bytes; ++i ){
        sym_var_uniq_name = sym_var_base_name + "_" + to_string(i);
        auto it = ACTIVE_SYM_VAR_NAMES.find(sym_var_uniq_name);
        if ( it != ACTIVE_SYM_VAR_NAMES.end() ){
            read_symbolic = true;
            operand_values.push_back(sym_var_uniq_name);
        }
        else{
            operand_values.push_back(get_bitvector_string_reg( reg_val, i, reg_obj->n_bytes ));
        }
    }
}

void get_string_values_mem_operand( ADDRINT base_address, Operand* mem_obj, vector<string>& operand_values, bool& read_symbolic ){
    for ( uint32_t i = 0; i < mem_obj->n_values; ++i ){
        ADDRINT effective_address = base_address + i * mem_obj->n_bytes;
        string sym_var_uniq_name = mem_to_sym_var_uniq_name(mem_obj, effective_address, false);
        auto it = ACTIVE_SYM_VAR_NAMES.find(sym_var_uniq_name);
        if ( it != ACTIVE_SYM_VAR_NAMES.end() ){
            read_symbolic = true;
            operand_values.push_back(sym_var_uniq_name);
        }
        else{
            operand_values.push_back(get_bitvector_string_mem( effective_address, mem_obj->n_bytes ));
        }
    }
}

string reg_to_sym_var_base_name( Operand* reg_obj, bool create_fresh ){
    string sym_var_base_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg));
    if ( create_fresh ){
        if ( SYM_VAR_COUNTERS.find(sym_var_base_name) == SYM_VAR_COUNTERS.end() ){
            SYM_VAR_COUNTERS[sym_var_base_name] = 0;
        }
        else{
            SYM_VAR_COUNTERS[sym_var_base_name]++;
        }
    }
    return sym_var_base_name + "_" + to_string(SYM_VAR_COUNTERS[sym_var_base_name]) + "_b" + to_string(reg_obj->n_bytes * 8);
}

string mem_to_sym_var_uniq_name( Operand* mem_obj, ADDRINT effective_address, bool create_fresh ){
    string sym_var_uniq_name = "mem_" + hexstr(effective_address);
    if ( create_fresh ){
        if ( SYM_VAR_COUNTERS.find(sym_var_uniq_name) == SYM_VAR_COUNTERS.end() ){
            SYM_VAR_COUNTERS[sym_var_uniq_name] = 0;
        }
        else{
            SYM_VAR_COUNTERS[sym_var_uniq_name]++;
        }
    }
    return sym_var_uniq_name + "_" + to_string(SYM_VAR_COUNTERS[sym_var_uniq_name]) + "_b" + to_string(mem_obj->n_bytes * 8);
}

void process_EV_generator_reg( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT n_values, PINTOOL_REGISTER* write_register ){

    if ( !GO ){
        return;
    }

    string containing_rtn_name = ROUTINE_NAME_MAP[rtn_id];

    ++INSTRUCTION_EXECUTION_COUNTS[rtn_id][ins_offset];

    vector<string> EV_generator_id;
    EV_generator_id.push_back(to_string(CURRENT_IO_VARS->first));
    EV_generator_id.push_back(containing_rtn_name);
    EV_generator_id.push_back(to_string(ins_offset));
    EV_generator_id.push_back(to_string(INSTRUCTION_EXECUTION_COUNTS[rtn_id][ins_offset]));
    EV_generator_id.push_back("");
    for ( ADDRINT i=0; i<n_values; ++i ){
        EV_generator_id.back() = to_string(i);

        size_t temp = EV_GENERATOR_HASH_FUNCTION(EV_generator_id);

        if ( MODE == PRE_EV_OVERWRITE_BASELINE ){
            string sym_var_name = "reg_" + REG_StringShort(REG_FullRegName(INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->write_vec_registers.back()->reg));
            sym_var_name = sym_var_name + "_" + to_string(SYM_VAR_COUNTERS[sym_var_name]) + "_b" + to_string(INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->write_vec_registers.back()->n_bytes * 8) + "_" + to_string(i);
            if ( ACTIVE_SYM_VAR_NAMES.find(sym_var_name) != ACTIVE_SYM_VAR_NAMES.end() ){
                IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].insert(temp);
                log2("saving as a generator site");
            }
        }
        else if ( EV_GENERATOR_HASH == 0 ){
            auto it = IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].find(temp);
            if ( it != IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].end() ) {
                N_HITS++;

                IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].erase(it);

                log2("Overwriting instruction output with EV");

                EV_GENERATOR_HASH = temp;
                if ( INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->write_vec_registers.back()->n_bytes == 8 ){
                    write_register->qword[i] = 0x7FF8000000000000;
                }
                else if ( INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->write_vec_registers.back()->n_bytes == 4 ){
                    write_register->dword[i] = 0x7FC00000;
                }
                else{
                    assert(false && "unsupported type for EV overwriting");
                }

                WRITE_BUFFER << "\n**EXCVATE: injection in " << containing_rtn_name << "::" << hexstr(ins_offset) << "\t" << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->disassembly << "\t" << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->src_location << "\n\n";
                for ( const auto &x : EV_generator_id ){
                    WRITE_BUFFER << x << " ";
                }
                WRITE_BUFFER << endl;
            }
        }
    }
}

void instrument_instruction( INS ins ){

    Instruction* ins_obj_ptr = construct_instruction_object(ins);
    ins_obj_ptr->smtlib2_op = opcode_to_smtlib2(INS_Opcode(ins));
    ADDRINT rtn_id = RTN_Id(INS_Rtn(ins));
    ADDRINT ins_offset = INS_Address(ins) - RTN_Address(INS_Rtn(ins));
    INSTRUCTION_OBJ_MAP[rtn_id][ins_offset] = ins_obj_ptr; 

#if VERBOSE >= 1
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)print_instruction, IARG_ADDRINT, rtn_id, IARG_ADDRINT, ins_offset, IARG_ADDRINT, INS_Opcode(ins), IARG_END);
#endif

    if ( MODE > BASELINE_ONLY ){
        if ( ins_obj_ptr->read_vec_registers.size() + ins_obj_ptr->write_vec_registers.size() != 0 ){
            if ( ins_obj_ptr->smtlib2_op == "$cmp" ){
                if ( (ins_obj_ptr->read_memory.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 1) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)cmp_readVM,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_MEMORYREAD_EA,
                        IARG_END
                    );
                }
                else if ( (ins_obj_ptr->read_memory.size() == 0) && (ins_obj_ptr->read_vec_registers.size() == 2) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)cmp_readVV,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.front()->reg,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_END
                    );
                }
                else{
                    log0(ins_obj_ptr->disassembly);
                    assert(false && "unexpected read operators for compare op");
                }
            }
            else{
                if ( (ins_obj_ptr->write_memory.size() == 1) && (ins_obj_ptr->write_vec_registers.size() == 0) && (ins_obj_ptr->read_vec_registers.size() == 1) && (ins_obj_ptr->read_memory.size() == 0) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)writeM_readV,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_MEMORYWRITE_EA,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_END
                    );
                }
                else if ( (ins_obj_ptr->write_memory.size() == 0) && (ins_obj_ptr->write_vec_registers.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 2) && (ins_obj_ptr->read_memory.size() == 0) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)writeV_readVV,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.front()->reg,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_END
                    );
                }
                else if ( (ins_obj_ptr->write_memory.size() == 0) && (ins_obj_ptr->write_vec_registers.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 1) && (ins_obj_ptr->read_memory.size() == 1) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)writeV_readVM,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_MEMORYREAD_EA,
                        IARG_END
                    );
                }
                else if ( (ins_obj_ptr->write_memory.size() == 0) && (ins_obj_ptr->write_vec_registers.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 1) && (ins_obj_ptr->read_memory.size() == 0) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)writeV_readV,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
                        IARG_END
                    );
                }
                else if ( (ins_obj_ptr->write_memory.size() == 0) && (ins_obj_ptr->write_vec_registers.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 0) && (ins_obj_ptr->read_memory.size() == 1) ){
                    INS_InsertCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR)writeV_readM,
                        IARG_ADDRINT, rtn_id,
                        IARG_ADDRINT, ins_offset,
                        IARG_MEMORYREAD_EA,
                        IARG_END
                    );
                }
            }
        }

        InjectableType t = is_EV_generator_reg(INS_Opcode(ins) );

        if ( t == ADDSUB || t == FMA ){
            INS_InsertCall(
                ins,
                IPOINT_BEFORE,
                (AFUNPTR)log_instruction,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_ADDRINT, INS_Opcode(ins),
                IARG_END
            );
        }
        else if ( t != NOT_INJECTABLE ){

            // added to catch a case in which a binary contains instructions with zmm registers;
            // the tool does not currently support AVX512
            if ( ins_obj_ptr->write_vec_registers.size() == 0 ){
                return;
            }

            // an assumption
            assert( INS_IsValidForIpointAfter(ins) && "instrumentation insertion not valid for after instruction");

            // using second read operand to determine n_values as VEX instructions can copy over values from the first read vector operand before zeroing them out
            INS_InsertCall(
                ins,
                IPOINT_AFTER,
                (AFUNPTR)process_EV_generator_reg,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_ADDRINT, ins_obj_ptr->read_operands.back()->n_values,
                IARG_REG_REFERENCE, ins_obj_ptr->write_vec_registers.back()->reg,
                IARG_END
            );
        }
    }
}

void intercept_stdout_and_stderr_writes( THREADID threadIndex, CONTEXT* ctxt, SYSCALL_STANDARD std, VOID* v ){
    ADDRINT sysnum = PIN_GetSyscallNumber(ctxt, std);
    if ( sysnum == SYS_write ){
        ADDRINT arg0 = PIN_GetSyscallArgument(ctxt, std, 0);
        if ( arg0 == 1 || arg0 == 2 ){
            if ( SYS_WRITE_INTERCEPT_FLAG == false ){
                SYS_WRITE_INTERCEPT_FLAG = true;
                WRITE_BUFFER << "\n================= BEGIN STDOUT / STDERR =================\n";
            }
            ADDRINT arg1 = PIN_GetSyscallArgument(ctxt, std, 1);
            ADDRINT arg2 = PIN_GetSyscallArgument(ctxt, std, 2);
            WRITE_BUFFER.write(reinterpret_cast<const char*>(arg1), arg2);
            PIN_SetSyscallNumber(ctxt, std, SYS_getpid);
        }
    }
}

void check_for_timeout(){
    if ( MODE == EV_OVERWRITE && GO ){
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_sec = end.tv_sec - START_TIME.tv_sec;
        double elapsed_nsec = end.tv_nsec - START_TIME.tv_nsec;
        if ( (elapsed_sec + elapsed_nsec / 1e9) > TIME_LIMIT ){
            GO = false;
            EXIT_CODE = 124;
            WRITE_BUFFER << "\n**EXCVATE: TIMEOUT\n\n";
            PIN_ExecuteAt(RETURN_CONTEXT);
        }
    }
}

void trace_pass( TRACE trace, VOID* v ){
    TRACE_InsertCall(
        trace,
        IPOINT_BEFORE,
        (AFUNPTR)check_for_timeout,
        IARG_END
    );
}

// Entry point for the tool
int main( int argc, char *argv[] ){

    KNOB< uint32_t > knob_mode(KNOB_MODE_WRITEONCE, "pintool", "m", "0", "(for internal use only)");
    KNOB< string > knob_prototype_path(KNOB_MODE_WRITEONCE, "pintool", "f", "", "(for internal use only)");
    KNOB< string > knob_error_handler_names(KNOB_MODE_WRITEONCE, "pintool", "e", "", "(for internal use only)");
    // KNOB< uint32_t > knob_skip(KNOB_MODE_WRITEONCE, "pintool", "s", "", "(for internal use only)");

    // Initialize Pin
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)){
        std::cerr << "Error during PIN_Init" << std::endl;
        assert(false);
    }

    MODE = knob_mode.Value();
    TARGETED_FUNC_STATIC_INFO = process_prototype_file(knob_prototype_path.Value());
    TARGETED_FUNC_DYNAMIC_INFO = new FuncDynamicInfo;
    TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values.resize(TARGETED_FUNC_STATIC_INFO->io_vars.size());
    TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_value.resize(TARGETED_FUNC_STATIC_INFO->io_vars.size());

    string buffer;
    istringstream iss(knob_error_handler_names);
    while ( iss >> buffer ){
        ERROR_HANDLER_NAMES.push_back(buffer);
    }

    buffer = "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/" + TARGETED_FUNC_STATIC_INFO->name + ".io_vars";
    MAX_IO_VAR_BYTES = load_io_var_map( SAVED_IO_VARS_MAP, buffer );    

    if ( SAVED_IO_VARS_MAP.size() == 0 ){
        cout << "                  No executions found in regression tests!!" << endl; 
        return 0;
    }

    // if ( knob_skip.Value() ){
    //     ifstream infile("__skip");
    //     string line;
    //     while ( getline(infile, line) ){
    //         SKIP.insert(line);
    //     }
    // }

    // Set up instrumentation functions
    IMG_AddInstrumentFunction(image_pass1, NULL);
    IMG_AddInstrumentFunction(image_pass2, NULL);
    PIN_AddSyscallEntryFunction(intercept_stdout_and_stderr_writes, NULL);
    TRACE_AddInstrumentFunction(trace_pass, NULL);

    // Start the application
    PIN_StartProgram();

    return 0;
}