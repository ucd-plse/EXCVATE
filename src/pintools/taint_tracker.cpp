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
#include <bitset>

#ifndef VERBOSE
#define VERBOSE 0
#endif

#define BACK_TRACE_SIZE 3

using namespace std;

enum Mode {
    BASELINE = 0,
    EVENT_TRACE = 1,
};

struct Event{
    pair<ADDRINT,ADDRINT> loc;
    string code;
    uint64_t taint_count;
};

// Globals
bool FORCE_STDOUT=false;
size_t CURRENT_IO_VARS_HASH = 0;
size_t CURRENT_EV_GENERATOR_HASH = 0;
string CURRENT_SMT_GENERATED_INPUT_FILE_SUFFIX;
map<uint32_t, vector<uint8_t>> CURRENT_VAR_IDX_TO_SMT_GENERATED_BYTES_MAP;
vector<uint8_t>::const_iterator CURRENT_IO_VAR_BYTES_IT;
bool SYS_WRITE_INTERCEPT_FLAG=false;
uint32_t MAX_IO_VAR_BYTES;
uint8_t* IO_VAR_BUFFER_BASE_PTR;
uint32_t IO_VAR_BUFFER_OFFSET = 0;
FuncStaticInfo* TARGETED_FUNC_STATIC_INFO;
FuncDynamicInfo* TARGETED_FUNC_DYNAMIC_INFO;
vector<string> ERROR_HANDLER_NAMES;
map<const size_t,vector<uint8_t>> SAVED_IO_VARS_MAP;
AFUNPTR TARGETED_FUNCTION_PTR;
map<ADDRINT, map<ADDRINT, Instruction*>> INSTRUCTION_OBJ_MAP;
map<ADDRINT, string> ROUTINE_NAME_MAP;
ostringstream WRITE_BUFFER;
bool GO=false;
uint32_t MODE;
int32_t EXIT_CODE = 0;
vector<ADDRINT> INJECTED_IO_VAR_PTRS;
vector<ADDRINT> CALL_STACK;
map<size_t,set<size_t>> IO_VARS_HASH_TO_EV_GENERATOR_HASHES_MAP;
map<pair<size_t,size_t>,map<string,map<uint32_t, vector<uint8_t>>>> IO_VAR_AND_EV_GENERATOR_HASH_TO_SMT_GENERATED_VALS_MAP;
map<ADDRINT,map<ADDRINT, ADDRINT>> INSTRUCTION_EXECUTION_COUNTS;
boost::hash<vector<string>> EV_GENERATOR_HASH_FUNCTION;
struct timespec START_TIME;
double TIME_LIMIT;
CONTEXT* RETURN_CONTEXT = NULL;
vector<Event> EVENT_LOG;
set<string> TAINTED;
set<string> ANTI_TAINTED;
vector<pair<ADDRINT,ADDRINT>> READ_ONLY_ADDRESSES;

// Forward Declarations
void image_pass1( IMG img, void *v );
void image_pass2( IMG img, void *v );
void trace_pass( TRACE trace, VOID* v );
void error_handler_called();
void perform_analysis_loop( CONTEXT *ctxt );
void reset_and_run( CONTEXT* ctxt );
template <typename T>
void post_call( T* return_value_ptr );
void instrument_instruction( INS ins );
void intercept_stdout_and_stderr_writes( THREADID threadIndex, CONTEXT* ctxt, SYSCALL_STANDARD std, VOID* v );
void save_return_context( const CONTEXT* ctxt );
void check_for_timeout();
void print_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode );
void inject_io_var( ADDRINT* actual_val_ptr, ADDRINT var_idx );
template <typename T>
vector<ADDRINT> check_for_EVs_mem( ADDRINT base_address, uint32_t n_values, uint32_t n_bytes, bool print=false );
void pre_call( ADDRINT rtn_id );
void check_write_operand( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* reg, ADDRINT w_base_address );
void mark_nonEVable_write_operand( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* reg, ADDRINT w_base_address );
void check_read_operands( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT r_base_address );
void check_w_reg( PINTOOL_REGISTER* reg_val, Instruction* ins_obj_ptr );
void check_w_mem( ADDRINT base_address, Instruction* ins_obj_ptr );
void mark_nonEVable_w_reg( PINTOOL_REGISTER* reg_val, Instruction* ins_obj_ptr );
void mark_nonEVable_w_mem( ADDRINT base_address, Instruction* ins_obj_ptr );
void check_r_reg( Operand* reg_obj );
void check_r_mem( ADDRINT base_address, Operand* mem_obj );

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

    if ( SYS_WRITE_INTERCEPT_FLAG == true ){
        SYS_WRITE_INTERCEPT_FLAG = false;
        WRITE_BUFFER << "\n================== END STDOUT / STDERR ==================\n";
    }

    // check the output vars that were injected earlier
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

    if ( MODE == BASELINE ){
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_sec = end.tv_sec - START_TIME.tv_sec;
        double elapsed_nsec = end.tv_nsec - START_TIME.tv_nsec;
        TIME_LIMIT = 100 * (elapsed_sec + elapsed_nsec / 1e9);
    }
    else if ( MODE == EVENT_TRACE && EVENT_LOG.size() > 0 ) {
        if ( EXIT_CODE == 0 || EXIT_CODE == 124 ){
            string out_file_path = "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/" + TARGETED_FUNC_STATIC_INFO->name + "." + to_string(CURRENT_IO_VARS_HASH) + "." + to_string(CURRENT_EV_GENERATOR_HASH) + ".smt2." + CURRENT_SMT_GENERATED_INPUT_FILE_SUFFIX + ".event_trace";
            ofstream out(out_file_path.c_str());
            out << WRITE_BUFFER.str() << endl;

            out << "\n====================================\n" << endl;
            out << left << setw(64) << "Assembly Location" << setw(50) << "Disassembly" << setw(100) << "Source Location" << setw(8) << "Event" << setw(15) << "Taint Count" << endl;
            if ( (EXIT_CODE == 124) && (EVENT_LOG.size() > 100)){
                for (uint32_t i=0; i<100; ++i) {
                    Instruction* ins = INSTRUCTION_OBJ_MAP[EVENT_LOG[i].loc.first][EVENT_LOG[i].loc.second];
                    out << setw(64) << left << ROUTINE_NAME_MAP[EVENT_LOG[i].loc.first] + "::" + hexstr(EVENT_LOG[i].loc.second) << setw(50) << ins->disassembly << setw(100) << ins->src_location << setw(8) << EVENT_LOG[i].code << setw(15) << EVENT_LOG[i].taint_count << endl;
                }
                out << "\n**EXCVATE: EVENT TRACE TRUNCATED\n\n" << endl;
            }
            else{
                for (uint32_t i=0; i<EVENT_LOG.size(); ++i) {
                    Instruction* ins = INSTRUCTION_OBJ_MAP[EVENT_LOG[i].loc.first][EVENT_LOG[i].loc.second];
                    out << setw(64) << left << ROUTINE_NAME_MAP[EVENT_LOG[i].loc.first] + "::" + hexstr(EVENT_LOG[i].loc.second) << setw(50) << ins->disassembly << setw(100) << ins->src_location << setw(8) << EVENT_LOG[i].code << setw(15) << EVENT_LOG[i].taint_count << endl;
                }
            }
        }
    }

    if ( MODE == EVENT_TRACE && FORCE_STDOUT ){
        cout << WRITE_BUFFER.str() << endl;

        cout << "\n====================================\n" << endl;
        cout << left << setw(30) << "Assembly Location" << setw(50) << "Disassembly" << setw(100) << "Source Location" << setw(8) << "Event" << setw(15) << "Taint Count" << endl;
        if ( (EXIT_CODE == 124) && (EVENT_LOG.size() > 100)){
            for (uint32_t i=0; i<100; ++i) {
                Instruction* ins = INSTRUCTION_OBJ_MAP[EVENT_LOG[i].loc.first][EVENT_LOG[i].loc.second];
                cout << setw(30) << left << ROUTINE_NAME_MAP[EVENT_LOG[i].loc.first] + "::" + hexstr(EVENT_LOG[i].loc.second) << setw(50) << ins->disassembly << setw(100) << ins->src_location << setw(8) << EVENT_LOG[i].code << setw(15) << EVENT_LOG[i].taint_count << endl;
            }
            cout << "\n**EXCVATE: EVENT TRACE TRUNCATED\n\n" << endl;
        }
        else{
            for (uint32_t i=0; i<EVENT_LOG.size(); ++i) {
                Instruction* ins = INSTRUCTION_OBJ_MAP[EVENT_LOG[i].loc.first][EVENT_LOG[i].loc.second];
                cout << setw(30) << left << ROUTINE_NAME_MAP[EVENT_LOG[i].loc.first] + "::" + hexstr(EVENT_LOG[i].loc.second) << setw(50) << ins->disassembly << setw(100) << ins->src_location << setw(8) << EVENT_LOG[i].code << setw(15) << EVENT_LOG[i].taint_count << endl;
            }
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
    if ( MODE == EVENT_TRACE && CURRENT_VAR_IDX_TO_SMT_GENERATED_BYTES_MAP.find(var_idx) != CURRENT_VAR_IDX_TO_SMT_GENERATED_BYTES_MAP.end() ){
        for ( uint32_t i=0; i < total_bytes; ++i ){
            uint8_t* temp = IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET;
            *temp = CURRENT_VAR_IDX_TO_SMT_GENERATED_BYTES_MAP[var_idx][i];
            ++CURRENT_IO_VAR_BYTES_IT;
            ++IO_VAR_BUFFER_OFFSET;
        }
    }
    else{
        for ( uint32_t i=0; i < total_bytes; ++i ){
            uint8_t* temp = IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET;
            *temp = *CURRENT_IO_VAR_BYTES_IT;
            ++CURRENT_IO_VAR_BYTES_IT;
            ++IO_VAR_BUFFER_OFFSET;
        }
    }

    uint32_t offset = 0;
    WRITE_BUFFER << "(in) " << TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].name << ": ";
    for ( uint32_t i=0; i < TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values[var_idx]; ++i ){
        if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == INTEGER ){
            WRITE_BUFFER << *(reinterpret_cast<int*>(IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes + offset)) << " ";
        }
        else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type == CHARACTER ){
            WRITE_BUFFER << *(reinterpret_cast<char*>(IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes + offset)) << " ";
        }
        else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].type >= REAL_IN ){
            if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 4 ){
                WRITE_BUFFER << *(reinterpret_cast<float*>(IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes + offset)) << " ";
            }
            else if ( TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes == 8 ){
                WRITE_BUFFER << *(reinterpret_cast<double*>(IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes + offset)) << " ";
            } 
        }
        offset = offset + TARGETED_FUNC_STATIC_INFO->io_vars[var_idx].n_bytes;
    }
    WRITE_BUFFER << endl;

    // replace the old val ptr and save the new ptr
    INJECTED_IO_VAR_PTRS.push_back((ADDRINT) IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET - total_bytes);
    *actual_val_ptr = INJECTED_IO_VAR_PTRS.back();

    // maintain alignment
    IO_VAR_BUFFER_OFFSET = IO_VAR_BUFFER_OFFSET + max(0, 4 - (int)total_bytes);

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
}

void pre_call( ADDRINT rtn_id ){
    CALL_STACK.push_back(rtn_id);
}

void save_return_context( const CONTEXT* ctxt ){
    if ( MODE == BASELINE ){
        delete RETURN_CONTEXT;
        RETURN_CONTEXT = new CONTEXT;
        PIN_SaveContext(ctxt, RETURN_CONTEXT);
    }
}

void reset_and_run( CONTEXT* ctxt ){

    CURRENT_IO_VAR_BYTES_IT = SAVED_IO_VARS_MAP[CURRENT_IO_VARS_HASH].begin();

    // construct MXCSR contents in the target program's address space
    IO_VAR_BUFFER_OFFSET = 0;
    for ( uint32_t i=0; i < 2; ++i ){
        uint8_t* temp = IO_VAR_BUFFER_BASE_PTR + IO_VAR_BUFFER_OFFSET;
        *temp = *CURRENT_IO_VAR_BYTES_IT;
        ++CURRENT_IO_VAR_BYTES_IT;
        ++IO_VAR_BUFFER_OFFSET;
    }
    ADDRINT mxcsr_contents = (ADDRINT) *(reinterpret_cast<uint16_t*>(IO_VAR_BUFFER_BASE_PTR));
    PIN_SetContextReg(ctxt, REG_MXCSR, mxcsr_contents);
    IO_VAR_BUFFER_OFFSET = 0; // now that MXCSR reg has been set, we reset offset so buffer will be filled with actual io vars

    CALL_STACK.clear();
    WRITE_BUFFER.str("");
    EXIT_CODE = 0;
    INSTRUCTION_EXECUTION_COUNTS.clear();
    INJECTED_IO_VAR_PTRS.clear();
    clock_gettime(CLOCK_MONOTONIC, &START_TIME);
    GO = true;
    EVENT_LOG.clear();
    TAINTED.clear();

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
    uint32_t io_var_buffer_size = MAX_IO_VAR_BYTES;
    for ( const auto& i : TARGETED_FUNC_STATIC_INFO->process_order ){
        if ( TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes % 4 != 0 ){
            io_var_buffer_size = io_var_buffer_size + max(0, 4 - (int)TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes);
        }
    }
    IO_VAR_BUFFER_BASE_PTR = (uint8_t*) malloc((io_var_buffer_size) * sizeof(uint8_t));

    for ( const auto& x : IO_VARS_HASH_TO_EV_GENERATOR_HASHES_MAP ){
        
        CURRENT_IO_VARS_HASH = x.first;

        // baseline run
        MODE = BASELINE;
        CURRENT_EV_GENERATOR_HASH = 0;
        log1("===================== " + to_string(CURRENT_IO_VARS_HASH) + " =====================");
        reset_and_run(ctxt);
        MODE = EVENT_TRACE;

        for ( const auto y : x.second ){
            CURRENT_EV_GENERATOR_HASH = y;
            for ( const auto& z : IO_VAR_AND_EV_GENERATOR_HASH_TO_SMT_GENERATED_VALS_MAP[make_pair(CURRENT_IO_VARS_HASH,CURRENT_EV_GENERATOR_HASH)] ){
                log1("===================== " + to_string(CURRENT_IO_VARS_HASH) + "." + to_string(CURRENT_EV_GENERATOR_HASH) + " =====================");
                CURRENT_SMT_GENERATED_INPUT_FILE_SUFFIX = z.first;
                CURRENT_VAR_IDX_TO_SMT_GENERATED_BYTES_MAP = z.second;
                reset_and_run(ctxt);
            }
        }
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
    if ( GO && MODE == EVENT_TRACE ){
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

        ifstream in_file;
        in_file.open( "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/__input_file_list" );
        string buffer;
        while ( getline(in_file, buffer) ){
            if ( buffer == "" ){
                break;
            }

            map<uint32_t, vector<uint8_t>> var_idx_to_smt_generated_bytes_map;

            ifstream in_file2;
            string buffer2;
            in_file2.open(buffer);
            while ( getline(in_file2, buffer2) ){
                auto find_loc = buffer2.find("__");
                if ( find_loc != string::npos ){
                    string var_name = "";
                    auto i = buffer2.find_last_of(' ', find_loc);
                    while ( ++i < find_loc ){
                        var_name.push_back(buffer2[i]);
                    }
                    uint32_t var_idx = TARGETED_FUNC_STATIC_INFO->var_name_to_idx[var_name];

                    string bitstring = "";
                    while ( true ){
                        find_loc = buffer2.find("#b", find_loc);
                        if ( find_loc == string::npos ){
                            break;
                        }
                        find_loc = find_loc + 2;
                        uint32_t i = 0;
                        while ( buffer2[find_loc + i] != ' ' && buffer2[find_loc + i] != ')' ){
                            assert( buffer2[find_loc + i] == '0' || buffer2[find_loc + i] == '1' );
                            bitstring.push_back(buffer2[find_loc + i]);
                            ++i;
                        }
                    }

                    if ( bitstring.size() == 64 ){
                        bitset<64> b(bitstring);
                        uint64_t int_representation = b.to_ullong();
                        for ( uint32_t i = 0; i < 8; ++i ){
                            var_idx_to_smt_generated_bytes_map[var_idx].push_back((reinterpret_cast<uint8_t*>(&int_representation)[i]));
                        }
                    }
                    else if ( bitstring.size() == 32 ){
                        bitset<32> b(bitstring);
                        uint32_t int_representation = b.to_ulong();
                        for ( uint32_t i = 0; i < 4; ++i ){
                            var_idx_to_smt_generated_bytes_map[var_idx].push_back((reinterpret_cast<uint8_t*>(&int_representation)[i]));
                        }
                    }
                    else{
                        assert(false && "bitstring length not equal to 32 or 64");
                    }
                }
            }

            if ( buffer.find("/") != string::npos ){
                buffer = buffer.substr(buffer.find_last_of('/') + 1, string::npos);
            }
            uint32_t i = 0;
            while ( i < buffer.size() ){
                ++i;
                if ( buffer[i] == '.' ){
                    buffer[i] = ' ';
                }
            }
            istringstream iss(buffer);
            iss >> buffer; // skip function name
            iss >> CURRENT_IO_VARS_HASH;
            iss >> CURRENT_EV_GENERATOR_HASH;
            iss >> buffer; // skip 'smt2'
            iss >> CURRENT_SMT_GENERATED_INPUT_FILE_SUFFIX;
            IO_VARS_HASH_TO_EV_GENERATOR_HASHES_MAP[CURRENT_IO_VARS_HASH].insert(CURRENT_EV_GENERATOR_HASH);
            IO_VAR_AND_EV_GENERATOR_HASH_TO_SMT_GENERATED_VALS_MAP[make_pair(CURRENT_IO_VARS_HASH,CURRENT_EV_GENERATOR_HASH)][CURRENT_SMT_GENERATED_INPUT_FILE_SUFFIX] = var_idx_to_smt_generated_bytes_map;
        }

        string saved_io_var_map_name = "__EXCVATE/" + TARGETED_FUNC_STATIC_INFO->name + "/" + TARGETED_FUNC_STATIC_INFO->name + ".io_vars";
        MAX_IO_VAR_BYTES = load_io_var_map( SAVED_IO_VARS_MAP, saved_io_var_map_name );    

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

        // make note of read-only sections so that we can later reason about read-only constants
        if ( SEC_IsReadable(sec) && !SEC_IsWriteable(sec) ){
            READ_ONLY_ADDRESSES.push_back(make_pair(SEC_Address(sec), SEC_Address(sec) + SEC_Size(sec)));
        }

        // if this section is executable, process any routines therein
        if ( SEC_IsExecutable(sec) ){
            for ( RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn) ){
                RTN_Open(rtn);

                ROUTINE_NAME_MAP[RTN_Id(rtn)] = RTN_Name(rtn);

                RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR) pre_call, IARG_ADDRINT, RTN_Id(rtn), IARG_END);
                RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR) save_return_context, IARG_CONST_CONTEXT, IARG_END);

                for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
                    instrument_instruction(ins);
                }

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

void check_write_operand( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* reg, ADDRINT w_base_address ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    if ( w_base_address != 0 ){
        check_w_mem(w_base_address, ins_obj_ptr);
    }
    else{
        check_w_reg(reg, ins_obj_ptr);
    }
}

void mark_nonEVable_write_operand( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* reg, ADDRINT w_base_address ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    if ( w_base_address != 0 ){
        mark_nonEVable_w_mem(w_base_address, ins_obj_ptr);
    }
    else{
        mark_nonEVable_w_reg(reg, ins_obj_ptr);
    }
}


void check_read_operands( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT r_base_address ){

    if (!GO ){
        return;
    }

    if ( (EVENT_LOG.size() > 0) && (EVENT_LOG.back().code == "---- ") ){
        EVENT_LOG.pop_back();
    }
    Event event = { make_pair(rtn_id,ins_offset), "---- ", TAINTED.size() };
    EVENT_LOG.push_back(event);

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    if ( r_base_address != 0 ){
        check_r_mem(r_base_address, ins_obj_ptr->read_memory.back());
    }
    for ( const auto& read_reg_obj_ptr : ins_obj_ptr->read_vec_registers ){
        check_r_reg(read_reg_obj_ptr);
    }

    // if the instruction does not write a float, then we can remove the anti-taint read event
    // since there will be no subsequent check of the write
    bool ins_writes_float = ( (ins_obj_ptr->write_memory.size() > 0) || (ins_obj_ptr->write_vec_registers.size() > 0) );
    if ( !ins_writes_float && EVENT_LOG.back().code[4] == 'A' ){
        EVENT_LOG.pop_back();
    }
}

void mark_nonEVable_w_reg( PINTOOL_REGISTER* reg_val, Instruction* ins_obj_ptr ){
    
    Operand* reg_obj = ins_obj_ptr->write_vec_registers.back();

    string reg_base_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8);
    for ( uint32_t i = 0; i < REG_Size(reg_obj->reg)/reg_obj->n_bytes; ++i ){
        
        string reg_uniq_name = reg_base_name + "_" + to_string(i);
        auto it = TAINTED.find(reg_uniq_name);
        bool w_reg_was_tainted = ( it != TAINTED.end() );

        if ( w_reg_was_tainted ){
            TAINTED.erase(it);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[2] = 'K';
            log2("Kill EV @ " + reg_uniq_name);
            log3("taint removed from " + reg_uniq_name);
        }

        double value = get_double_value_reg( reg_val, i, reg_obj->n_bytes );
        bool w_reg_is_EV = ( isnan(value) || isinf(value) );
        
        if ( w_reg_is_EV ){
            ANTI_TAINTED.insert(reg_uniq_name);
            log3("anti-taint added to " + reg_uniq_name);
        }

        // check other interpretations of the values represented in the register and assign anti-taint as necessary
        if ( reg_obj->n_bytes == 8 ){
            for ( uint32_t j = 2*i; j < 2*i+2; ++j ){

                value = get_double_value_reg( reg_val, j, reg_obj->n_bytes / 2 );
                w_reg_is_EV = ( isnan(value) || isinf(value) );
                
                if ( w_reg_is_EV ){
                    reg_uniq_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8 / 2) + "_" + to_string(j);
                    ANTI_TAINTED.insert(reg_uniq_name);
                    log3("anti-taint added to " + reg_uniq_name);
                }
            }
        }
        else if ( (reg_obj->n_bytes == 4) && (i%2 == 0) ){

            value = get_double_value_reg( reg_val, i/2, reg_obj->n_bytes * 2 );
            w_reg_is_EV = ( isnan(value) || isinf(value) );
            
            if ( w_reg_is_EV ){
                reg_uniq_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8 * 2) + "_" + to_string(i/2);
                ANTI_TAINTED.insert(reg_uniq_name);
                log3("anti-taint added to " + reg_uniq_name);
            }
        }
    }
    
    if ( (EVENT_LOG.back().code == "---- ") || (EVENT_LOG.back().code[4] == 'A') ){
        EVENT_LOG.pop_back();
    }
}

void mark_nonEVable_w_mem( ADDRINT base_address, Instruction* ins_obj_ptr ){

    Operand* mem_obj = ins_obj_ptr->write_memory.back();    

    for ( uint32_t i = 0; i < mem_obj->n_values; ++i ){

        ADDRINT effective_address = base_address + i * mem_obj->n_bytes;
        
        string mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string(mem_obj->n_bytes * 8);
        auto it = TAINTED.find(mem_uniq_name);
        bool w_mem_was_tainted = ( it != TAINTED.end() );

        if ( w_mem_was_tainted ){
            TAINTED.erase(it);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[2] = 'K';
            log2("Kill EV @ " + mem_uniq_name);
            log3("taint removed from " + mem_uniq_name);
        }

        double value = get_double_value_mem( effective_address, mem_obj->n_bytes );
        bool w_mem_is_EV = ( isnan(value) || isinf(value) );

        if ( w_mem_is_EV ){
            ANTI_TAINTED.insert(mem_uniq_name);
            log3("anti-taint added to " + mem_uniq_name);
        }

        // check other interpretations of the values represented in memory and assign anti-taint as necessary
        if ( mem_obj->n_bytes == 8 ){
            for ( uint32_t j = 2*i; j < 2*i+2; ++j ){
                
                effective_address = base_address + j * (mem_obj->n_bytes / 2);
                value = get_double_value_mem( effective_address, (mem_obj->n_bytes / 2) );
                w_mem_is_EV = ( isnan(value) || isinf(value) );
                
                if ( w_mem_is_EV ){
                    mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string((mem_obj->n_bytes * 8 / 2));
                    ANTI_TAINTED.insert(mem_uniq_name);
                    log3("anti-taint added to " + mem_uniq_name);
                }
            }
        }
        else if ( (mem_obj->n_bytes == 4) && (i%2 == 0) ){

            effective_address = base_address + (i/2) * (mem_obj->n_bytes * 2);
            value = get_double_value_mem( effective_address, (mem_obj->n_bytes * 2) );
            w_mem_is_EV = ( isnan(value) || isinf(value) );

            if ( w_mem_is_EV ){
                mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string((mem_obj->n_bytes * 8 * 2));
                ANTI_TAINTED.insert(mem_uniq_name);
                log3("anti-taint added to " + mem_uniq_name);
            }
        }
    }
    
    if ( (EVENT_LOG.back().code == "---- ") || (EVENT_LOG.back().code[4] == 'A') ){
        EVENT_LOG.pop_back();
    }
}

void check_w_reg( PINTOOL_REGISTER* reg_val, Instruction* ins_obj_ptr ){

    Operand* reg_obj = ins_obj_ptr->write_vec_registers.back();

    bool r_operand_was_tainted = ( EVENT_LOG.back().code[3] == 'r' );
    bool r_operand_was_antitainted = ( EVENT_LOG.back().code[4] == 'A' );
    
    string reg_base_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8);
    for ( uint32_t i = 0; i < REG_Size(reg_obj->reg)/reg_obj->n_bytes; ++i ){
        
        string reg_uniq_name = reg_base_name + "_" + to_string(i);

        bool w_reg_was_tainted = false;
        auto it = ANTI_TAINTED.find(reg_uniq_name);
        if ( it != ANTI_TAINTED.end() ){
            ANTI_TAINTED.erase(it);
            log3("anti-taint removed from " + reg_uniq_name);
        }
        else{
            it = TAINTED.find(reg_uniq_name);
            w_reg_was_tainted = ( it != TAINTED.end() );
            if ( w_reg_was_tainted ){
                TAINTED.erase(it);
            }
        }
        
        double value = get_double_value_reg( reg_val, i, reg_obj->n_bytes );
        bool w_reg_is_EV = ( isnan(value) || isinf(value) );
        
        if ( w_reg_is_EV && r_operand_was_antitainted ){
            if ( is_bitwise_and(ins_obj_ptr->opcode) ){
                TAINTED.insert(reg_uniq_name);
                EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
                EVENT_LOG.back().code[1] = 'P';
                log2("Prop EV @ " + reg_uniq_name);
                log3("taint added to " + reg_uniq_name);
            }
            else{
                ANTI_TAINTED.insert(reg_uniq_name);
                log3("anti-taint added to " + reg_uniq_name);
            }
        }
        else if ( w_reg_is_EV && r_operand_was_tainted ){
            TAINTED.insert(reg_uniq_name);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[1] = 'P';
            log2("Prop EV @ " + reg_uniq_name);
            log3("taint added to " + reg_uniq_name);
        }
        else if ( w_reg_is_EV && !r_operand_was_tainted ){
            TAINTED.insert(reg_uniq_name);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[0] = 'G';
            log2("Gen EV @ " + reg_uniq_name);
            log3("taint added to " + reg_uniq_name);
        }
        else if ( !w_reg_is_EV && w_reg_was_tainted ){
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[2] = 'K';
            log2("Kill EV @ " + reg_uniq_name);
            log3("taint removed from " + reg_uniq_name);
        }

        // check other interpretations of the values represented in memory and assign anti-taint as necessary
        if ( reg_obj->n_bytes == 8 ){
            for ( uint32_t j = 2*i; j < 2*i+2; ++j ){
                
                reg_uniq_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8 / 2) + "_" + to_string(j);                

                it = ANTI_TAINTED.find(reg_uniq_name);
                if ( it != ANTI_TAINTED.end() ){
                    ANTI_TAINTED.erase(it);
                    log3("anti-taint removed from " + reg_uniq_name);
                }

                value = get_double_value_reg( reg_val, j, reg_obj->n_bytes / 2 );
                w_reg_is_EV = ( isnan(value) || isinf(value) );
                if ( w_reg_is_EV && r_operand_was_antitainted ){
                    ANTI_TAINTED.insert(reg_uniq_name);
                    log3("anti-taint added to " + reg_uniq_name);
                }
            }
        }
        else if ( (reg_obj->n_bytes == 4) && (i%2 == 0) ){
                
            reg_uniq_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8 * 2) + "_" + to_string(i/2);                
            
            it = ANTI_TAINTED.find(reg_uniq_name);
            if ( it != ANTI_TAINTED.end() ){
                ANTI_TAINTED.erase(it);
                log3("anti-taint removed from " + reg_uniq_name);
            }

            value = get_double_value_reg( reg_val, i/2, reg_obj->n_bytes * 2 );
            w_reg_is_EV = ( isnan(value) || isinf(value) );
            if ( w_reg_is_EV && r_operand_was_antitainted ){
                ANTI_TAINTED.insert(reg_uniq_name);
                log3("anti-taint added to " + reg_uniq_name);
            }
        }
    }
    
    if ( (EVENT_LOG.back().code == "---- ") || (EVENT_LOG.back().code == "----A") ){
        EVENT_LOG.pop_back();
    }
    else if( EVENT_LOG.back().code[4] == 'A' ){
        EVENT_LOG.back().code[4] = ' ';
    }
}

void check_w_mem( ADDRINT base_address, Instruction* ins_obj_ptr ){

    Operand* mem_obj = ins_obj_ptr->write_memory.back();

    bool r_operand_was_tainted = ( EVENT_LOG.back().code[3] == 'r' );
    bool r_operand_was_antitainted = ( EVENT_LOG.back().code[4] == 'A' );

    for ( uint32_t i = 0; i < mem_obj->n_values; ++i ){

        ADDRINT effective_address = base_address + i * mem_obj->n_bytes;
        
        string mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string(mem_obj->n_bytes * 8);
        
        bool w_mem_was_tainted = false;
        auto it = ANTI_TAINTED.find(mem_uniq_name);
        if ( it != ANTI_TAINTED.end() ){
            ANTI_TAINTED.erase(it);
            log3("anti-taint removed from " + mem_uniq_name);
        }
        else{
            it = TAINTED.find(mem_uniq_name);
            w_mem_was_tainted = ( it != TAINTED.end() );
            if ( w_mem_was_tainted ){
                TAINTED.erase(it);
            }
        }

        double value = get_double_value_mem( effective_address, mem_obj->n_bytes );
        bool w_mem_is_EV = ( isnan(value) || isinf(value) );

        if ( w_mem_is_EV && r_operand_was_antitainted ){
            if ( is_bitwise_and(ins_obj_ptr->opcode) ){
                TAINTED.insert(mem_uniq_name);
                EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
                EVENT_LOG.back().code[1] = 'P';
                log2("Prop EV @ " + mem_uniq_name);
                log3("taint added to " + mem_uniq_name);
            }
            else{
                ANTI_TAINTED.insert(mem_uniq_name);
                log3("anti-taint added to " + mem_uniq_name);
            }
        }
        else if ( w_mem_is_EV && r_operand_was_tainted ){
            TAINTED.insert(mem_uniq_name);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[1] = 'P';
            log2("Prop EV @ " + mem_uniq_name);
            log3("taint added to " + mem_uniq_name);
        }
        else if ( w_mem_is_EV && !r_operand_was_tainted ){
            TAINTED.insert(mem_uniq_name);
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[0] = 'G';
            log2("Gen EV @ " + mem_uniq_name);
            log3("taint added to " + mem_uniq_name);
        }
        else if ( !w_mem_is_EV && w_mem_was_tainted ){
            EVENT_LOG.back().taint_count = TAINTED.size(); // for ( const auto& x : TAINTED ){ log0(x); }
            EVENT_LOG.back().code[2] = 'K';
            log2("Kill EV @ " + mem_uniq_name);
            log3("taint removed from " + mem_uniq_name);
        }

        // check other interpretations of the values represented in memory and assign anti-taint as necessary
        if ( mem_obj->n_bytes == 8 ){
            for ( uint32_t j = 2*i; j < 2*i+2; ++j ){
                
                effective_address = base_address + j * (mem_obj->n_bytes / 2);
                mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string(mem_obj->n_bytes * 8 / 2);

                it = ANTI_TAINTED.find(mem_uniq_name);
                if ( it != ANTI_TAINTED.end() ){
                    ANTI_TAINTED.erase(it);
                    log3("anti-taint removed from " + mem_uniq_name);
                }
                
                value = get_double_value_mem( effective_address, mem_obj->n_bytes / 2 );
                w_mem_is_EV = ( isnan(value) || isinf(value) );
                if ( w_mem_is_EV && r_operand_was_antitainted ){
                    ANTI_TAINTED.insert(mem_uniq_name);
                    log3("anti-taint added to " + mem_uniq_name);
                }
            }
        }
        else if ( (mem_obj->n_bytes == 4) && (i%2 == 0) ){
                
            effective_address = base_address + (i/2) * (mem_obj->n_bytes * 2);
            mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string(mem_obj->n_bytes * 8 * 2);

            it = ANTI_TAINTED.find(mem_uniq_name);
            if ( it != ANTI_TAINTED.end() ){
                ANTI_TAINTED.erase(it);
                log3("anti-taint removed from " + mem_uniq_name);
            }
            
            value = get_double_value_mem( effective_address, mem_obj->n_bytes * 2 );
            w_mem_is_EV = ( isnan(value) || isinf(value) );
            if ( w_mem_is_EV && r_operand_was_antitainted ){
                ANTI_TAINTED.insert(mem_uniq_name);
                log3("anti-taint added to " + mem_uniq_name);
            }
        }
    }
    
    if ( (EVENT_LOG.back().code == "---- ") || (EVENT_LOG.back().code[4] == 'A') ){
        EVENT_LOG.pop_back();
    }
}

void check_r_reg( Operand* reg_obj ){
    string reg_base_name = "reg_" + REG_StringShort(REG_FullRegName(reg_obj->reg)) + "_b" + to_string(reg_obj->n_bytes * 8);
    for ( uint32_t i = 0; i < REG_Size(reg_obj->reg)/reg_obj->n_bytes; ++i ){
        string reg_uniq_name = reg_base_name + "_" + to_string(i);        
        auto it = ANTI_TAINTED.find(reg_uniq_name);
        if ( it != ANTI_TAINTED.end() ){
            EVENT_LOG.back().code[4] = 'A';
            log3("anti-taint found @ " + reg_uniq_name);
        }
        else{
            it = TAINTED.find(reg_uniq_name);
            if ( it != TAINTED.end() ){
                log2("Read EV @ " + reg_uniq_name);
                EVENT_LOG.back().code[3] = 'r';
            }
        }
    }
}

void check_r_mem( ADDRINT base_address, Operand* mem_obj ){
    for ( uint32_t i = 0; i < mem_obj->n_values; ++i ){
        ADDRINT effective_address = base_address + i * mem_obj->n_bytes;
        string mem_uniq_name = "mem_" + hexstr(effective_address) + "_b" + to_string(mem_obj->n_bytes * 8);

        // assume that read only memory does not contain EVs
        for ( const auto& read_only_section_boundaries : READ_ONLY_ADDRESSES ){
            if ( (effective_address >= read_only_section_boundaries.first) && (effective_address <= read_only_section_boundaries.second) ){
                ANTI_TAINTED.insert(mem_uniq_name);
                log3("anti-taint added to " + mem_uniq_name);
            }
        }

        auto it = ANTI_TAINTED.find(mem_uniq_name);
        if ( it != ANTI_TAINTED.end() ){
            EVENT_LOG.back().code[4] = 'A';
            log3("anti-taint found @ " + mem_uniq_name);
        }
        else{
            it = TAINTED.find(mem_uniq_name);
            if ( it != TAINTED.end() ){
                log2("Read EV @ " + mem_uniq_name);
                EVENT_LOG.back().code[3] = 'r';
            }
        }
    }
}

#if VERBOSE >= 3
void debug_readVV( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg1_val, PINTOOL_REGISTER* r_reg2_val ){

    if (!GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    double value;
    for ( uint32_t i = 0; i < REG_Size(ins_obj_ptr->read_vec_registers.front()->reg)/ins_obj_ptr->read_vec_registers.front()->n_bytes; ++i ){
        value = get_double_value_reg( r_reg1_val, i, ins_obj_ptr->read_vec_registers.front()->n_bytes );
        log3(REG_StringShort(REG_FullRegName(ins_obj_ptr->read_vec_registers.front()->reg)) + "[" + to_string(i) + "]: " + to_string(value));
    }
    for ( uint32_t i = 0; i < REG_Size(ins_obj_ptr->read_vec_registers.back()->reg)/ins_obj_ptr->read_vec_registers.back()->n_bytes; ++i ){
        value = get_double_value_reg( r_reg2_val, i, ins_obj_ptr->read_vec_registers.back()->n_bytes );
        log3(REG_StringShort(REG_FullRegName(ins_obj_ptr->read_vec_registers.back()->reg)) + "[" + to_string(i) + "]: " + to_string(value));
    }
}

void debug_readVM( ADDRINT rtn_id, ADDRINT ins_offset, PINTOOL_REGISTER* r_reg_val, ADDRINT r_base_address ){

    if (!GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    double value;
    for ( uint32_t i = 0; i < REG_Size(ins_obj_ptr->read_vec_registers.back()->reg)/ins_obj_ptr->read_vec_registers.back()->n_bytes; ++i ){
        value = get_double_value_reg( r_reg_val, i, ins_obj_ptr->read_vec_registers.back()->n_bytes );
        log3(REG_StringShort(REG_FullRegName(ins_obj_ptr->read_vec_registers.back()->reg)) + "[" + to_string(i) + "]: " + to_string(value));
    }
    for ( uint32_t i = 0; i < ins_obj_ptr->read_memory.back()->n_values; ++i ){
        value = get_double_value_mem( r_base_address + i * ins_obj_ptr->read_memory.back()->n_bytes, ins_obj_ptr->read_memory.back()->n_bytes );
        log3(hexstr(r_base_address + i * ins_obj_ptr->read_memory.back()->n_bytes) + ": " + to_string(value));
    }
}

void debug_readV( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT w_mem_base_address,  PINTOOL_REGISTER* r_reg_val ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    double value;
    for ( uint32_t i = 0; i < REG_Size(ins_obj_ptr->read_vec_registers.back()->reg)/ins_obj_ptr->read_vec_registers.back()->n_bytes; ++i ){
        value = get_double_value_reg( r_reg_val, i, ins_obj_ptr->read_vec_registers.back()->n_bytes );
        log3(REG_StringShort(REG_FullRegName(ins_obj_ptr->read_vec_registers.back()->reg)) + "[" + to_string(i) + "]: " + to_string(value));
    }
}

void debug_readM( ADDRINT rtn_id, ADDRINT ins_offset,  ADDRINT r_base_address ){

    if ( !GO ){
        return;
    }

    Instruction* ins_obj_ptr = INSTRUCTION_OBJ_MAP[rtn_id][ins_offset];

    double value;
    for ( uint32_t i = 0; i < ins_obj_ptr->read_memory.back()->n_values; ++i ){
        value = get_double_value_mem( r_base_address + i * ins_obj_ptr->read_memory.back()->n_bytes, ins_obj_ptr->read_memory.back()->n_bytes );
        log3(hexstr(r_base_address + i * ins_obj_ptr->read_memory.back()->n_bytes) + ": " + to_string(value));
    }
}
#endif

void instrument_instruction( INS ins ){

    Instruction* ins_obj_ptr = construct_instruction_object(ins);
    ADDRINT rtn_id = RTN_Id(INS_Rtn(ins));
    ADDRINT ins_offset = INS_Address(ins) - RTN_Address(INS_Rtn(ins));
    INSTRUCTION_OBJ_MAP[rtn_id][ins_offset] = ins_obj_ptr; 

#if VERBOSE >= 1
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)print_instruction, IARG_ADDRINT, rtn_id, IARG_ADDRINT, ins_offset, IARG_ADDRINT, INS_Opcode(ins), IARG_END);
#endif

    if ( ins_obj_ptr->read_vec_registers.size() + ins_obj_ptr->write_vec_registers.size() == 0 ){
        return;
    }

    // since we do not support AVX2 or higher at this point, we do not need to worry about 
    // gather or scatter instructions
    if ( INS_IsVgather(ins) || INS_IsVscatter(ins) ){
        return;
    }

#if VERBOSE >= 3
    if ( (ins_obj_ptr->read_memory.size() == 1) && (ins_obj_ptr->read_vec_registers.size() == 1) ){
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)debug_readVM,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
            IARG_MEMORYREAD_EA,
            IARG_END
        );
    }
    else if ( ins_obj_ptr->read_vec_registers.size() == 2 ){
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)debug_readVV,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.front()->reg,
            IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
            IARG_END
        );
    }
    else if ( ins_obj_ptr->read_vec_registers.size() == 1 ){
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)debug_readV,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_REG_CONST_REFERENCE, ins_obj_ptr->read_vec_registers.back()->reg,
            IARG_END
        );
    }
    else if ( ins_obj_ptr->read_memory.size() == 1 ){
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)debug_readM,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_MEMORYREAD_EA,
            IARG_END
        );
    }
#endif

    if ( ins_obj_ptr->read_memory.size() > 0 ){
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)check_read_operands,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_MEMORYREAD_EA,
            IARG_END
        );
    }
    else{
        INS_InsertCall(
            ins,
            IPOINT_BEFORE,
            (AFUNPTR)check_read_operands,
            IARG_ADDRINT, rtn_id,
            IARG_ADDRINT, ins_offset,
            IARG_ADDRINT, 0,
            IARG_END
        );
    }

    if ( is_non_EVable(INS_Opcode(ins)) ){
        if ( ins_obj_ptr->write_vec_registers.size() == 1 ){

            assert( ins_obj_ptr->write_memory.size() == 0 );

            INS_InsertCall(
                ins,
                IPOINT_AFTER,
                (AFUNPTR)mark_nonEVable_write_operand,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_REG_CONST_REFERENCE, ins_obj_ptr->write_vec_registers.back()->reg,
                IARG_ADDRINT, 0,
                IARG_END
            );
        }
        else if ( ins_obj_ptr->write_memory.size() == 1 ){

            assert ( ins_obj_ptr->write_vec_registers.size() == 0 );

            INS_InsertCall(
                ins,
                IPOINT_AFTER,
                (AFUNPTR)mark_nonEVable_write_operand,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_REG_CONST_REFERENCE, REG_XMM0, // not referenced
                IARG_MEMORYOP_EA, ins_obj_ptr->write_memory.back()->memory_operand_idx,
                IARG_END
            );
        }
    }
    else{
        if ( ins_obj_ptr->write_vec_registers.size() == 1 ){

            assert( ins_obj_ptr->write_memory.size() == 0 );

            INS_InsertCall(
                ins,
                IPOINT_AFTER,
                (AFUNPTR)check_write_operand,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_REG_CONST_REFERENCE, ins_obj_ptr->write_vec_registers.back()->reg,
                IARG_ADDRINT, 0,
                IARG_END
            );
        }
        else if ( ins_obj_ptr->write_memory.size() == 1 ){

            assert ( ins_obj_ptr->write_vec_registers.size() == 0 );

            INS_InsertCall(
                ins,
                IPOINT_AFTER,
                (AFUNPTR)check_write_operand,
                IARG_ADDRINT, rtn_id,
                IARG_ADDRINT, ins_offset,
                IARG_REG_CONST_REFERENCE, REG_XMM0, // not referenced
                IARG_MEMORYOP_EA, ins_obj_ptr->write_memory.back()->memory_operand_idx,
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
    if ( MODE == EVENT_TRACE && GO ){
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

    KNOB< string > knob_prototype_path(KNOB_MODE_WRITEONCE, "pintool", "f", "", "(for internal use only)");
    KNOB< string > knob_error_handler_names(KNOB_MODE_WRITEONCE, "pintool", "e", "", "(for internal use only)");
    KNOB< uint32_t > knob_force_stdout(KNOB_MODE_WRITEONCE, "pintool", "x", "0", "(for internal use only)");

    // Initialize Pin
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)){
        std::cerr << "Error during PIN_Init" << std::endl;
        assert(false);
    }

    TARGETED_FUNC_STATIC_INFO = process_prototype_file(knob_prototype_path.Value());
    TARGETED_FUNC_DYNAMIC_INFO = new FuncDynamicInfo;
    TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_n_values.resize(TARGETED_FUNC_STATIC_INFO->io_vars.size());
    TARGETED_FUNC_DYNAMIC_INFO->io_var_idx_to_value.resize(TARGETED_FUNC_STATIC_INFO->io_vars.size());

    string buffer;
    istringstream iss(knob_error_handler_names);
    while ( iss >> buffer ){
        ERROR_HANDLER_NAMES.push_back(buffer);
    }

    if ( knob_force_stdout.Value() ){
        FORCE_STDOUT=true;
    }

    // Set up instrumentation functions
    IMG_AddInstrumentFunction(image_pass1, NULL);
    IMG_AddInstrumentFunction(image_pass2, NULL);
    PIN_AddSyscallEntryFunction(intercept_stdout_and_stderr_writes, NULL);
    TRACE_AddInstrumentFunction(trace_pass, NULL);

    // Start the application
    PIN_StartProgram();

    return 0;
}