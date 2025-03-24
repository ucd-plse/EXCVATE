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

// #define EVS_IN_INPUTS
#define BACK_TRACE_SIZE 3

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
    uint32_t io_var_buffer_size = MAX_IO_VAR_BYTES;
    for ( const auto& i : TARGETED_FUNC_STATIC_INFO->process_order ){
        if ( TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes % 4 != 0 ){
            io_var_buffer_size = io_var_buffer_size + max(0, 4 - (int)TARGETED_FUNC_STATIC_INFO->io_vars[i].n_bytes);
        }
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

                if ( VERBOSE >= 1 ){
                    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
                        instrument_instruction(ins);
                    }
                }
                else{
                    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
                        if ( is_EV_generator_reg(INS_Opcode(ins)) ){
                            instrument_instruction(ins);
                        }
                    }
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

void log_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode ){
    if ( GO ){
        string containing_rtn_name = ROUTINE_NAME_MAP[rtn_id];
        ostringstream oss;
        oss << setw(30) << left << containing_rtn_name + "::" + hexstr(ins_offset) << setw(6) << opcode << setw(50) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->disassembly << setw(100) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->src_location;
        log0(oss.str());
    }
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
            IO_VARS_HASH_TO_EV_GENERATOR_HASH_MAP[CURRENT_IO_VARS->first].insert(temp);
            log2("saving as a generator site");
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
    ADDRINT rtn_id = RTN_Id(INS_Rtn(ins));
    ADDRINT ins_offset = INS_Address(ins) - RTN_Address(INS_Rtn(ins));
    INSTRUCTION_OBJ_MAP[rtn_id][ins_offset] = ins_obj_ptr; 

#if VERBOSE >= 1
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)print_instruction, IARG_ADDRINT, rtn_id, IARG_ADDRINT, ins_offset, IARG_ADDRINT, INS_Opcode(ins), IARG_END);
#endif

    if ( MODE > BASELINE_ONLY ){

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