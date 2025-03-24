#include "EXCVATE_utils.h"
#include <boost/functional/hash.hpp>
#include <set>

#ifndef VERBOSE
#define VERBOSE 0
#endif

#define RESTRICTIVE
#define IO_VAR_MAX_INPUT_FLOATS 32
using namespace std;

map<ADDRINT, FuncStaticInfo*> FUNC_STATIC_INFO_MAP;
vector<FuncDynamicInfo*> FUNC_DYNAMIC_INFO_STACK;
map<ADDRINT,map<const size_t,vector<uint8_t>>> SAVED_IO_VARS_MAPS;
uint32_t EXECUTION_FILTER_COUNT = 0;
map<size_t, uint64_t> INS_EXECUTION_COUNTS;
map<size_t, uint64_t> EVABLE_INS_EXECUTION_COUNTS;
map<size_t, uint64_t> NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS;
set<string> x87_INSTRUCTIONS_EXECUTED;
set<string> UNSUPPORTED_INSTRUCTIONS;
#if VERBOSE >= 2
map<ADDRINT, map<ADDRINT, Instruction*>> INSTRUCTION_OBJ_MAP;
map<ADDRINT, string> ROUTINE_NAME_MAP;
#endif

void log_x87_ins();
void log_unsupported_instruction();
void pre_call( ADDRINT rtn_address, ADDRINT return_ip, CONTEXT* ctxt);
void process_io_var( ADDRINT rtn_address, ADDRINT val_base_ptr, ADDRINT var_idx );
void basic_filter( ADDRINT rtn_address );
void post_call( ADDRINT ip );
void image_load1( IMG img, VOID *v );
void image_load2( IMG img, VOID *v );
ADDRINT if_target_of_return( ADDRINT ip );
ADDRINT if_go();
void ins_count( ADDRINT rtn_address, ADDRINT ins_offset );
void EVable_ins_count( ADDRINT rtn_address, ADDRINT ins_offset );
void non_targeted_EVable_ins_count( ADDRINT rtn_address, ADDRINT ins_offset );
void save_execution_count_map_to_file(const map<size_t, uint64_t>& execution_count_map, const string& file_name);
void load_execution_count_map_from_file(map<size_t, uint64_t>& execution_count_map, const string& file_name);
void log_fork_in_control_flow( bool branch_taken, ADDRINT taken_address, ADDRINT fallthrough_address );
boost::hash<vector<uint8_t>> BYTE_HASH;
boost::hash<string> STRING_HASH;

#if VERBOSE >= 2
void print_instruction( ADDRINT rtn_id, ADDRINT ins_offset, ADDRINT opcode ){
    string containing_rtn_name = ROUTINE_NAME_MAP[rtn_id];
    ostringstream oss;
    oss << setw(30) << left << containing_rtn_name + "::" + hexstr(ins_offset) << setw(6) << opcode << setw(50) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->disassembly << setw(100) << INSTRUCTION_OBJ_MAP[rtn_id][ins_offset]->src_location;
    log1(oss.str());
}
#endif

void pre_call( ADDRINT rtn_address, ADDRINT return_ip, CONTEXT* ctxt ){

    log1("============= " + FUNC_STATIC_INFO_MAP[rtn_address]->name + " =============");
    log1("returning to " + hexstr(return_ip));

    FuncDynamicInfo* func_dynamic_info_ptr = new FuncDynamicInfo;
    func_dynamic_info_ptr->io_var_idx_to_n_values.resize(FUNC_STATIC_INFO_MAP[rtn_address]->io_vars.size());
    func_dynamic_info_ptr->io_var_idx_to_value.resize(FUNC_STATIC_INFO_MAP[rtn_address]->io_vars.size());
    func_dynamic_info_ptr->address = rtn_address;
    func_dynamic_info_ptr->return_ip = return_ip;

    // save MXCSR bits so we can reset them for future executions
    uint16_t mxcsr_contents = (uint16_t) PIN_GetContextReg(ctxt, REG_MXCSR);
    ADDRINT mxcsr_contents_ptr = (ADDRINT) &mxcsr_contents;
    mxcsr_contents &= ~0x3F; // Clear bits 0-5 corresponding to any previously triggered exceptions
    for ( uint32_t i=0; i < 2; ++i ){
        func_dynamic_info_ptr->io_var_bytes.push_back(*(reinterpret_cast<uint8_t*>(mxcsr_contents_ptr + i)));
    }

#ifdef RESTRICTIVE
    func_dynamic_info_ptr->unique_id = new vector<uint8_t>;
#endif

    if ( FUNC_DYNAMIC_INFO_STACK.size() > 1 && FUNC_DYNAMIC_INFO_STACK.back()->address == rtn_address ){
        log1("skipping processing of recursive call");
        func_dynamic_info_ptr->skip = true;
    }
    FUNC_DYNAMIC_INFO_STACK.push_back(func_dynamic_info_ptr);
}

// this methodology might require some rethinking with nested function calls of interest and unique ids which incorporate control flow changes
void basic_filter( ADDRINT rtn_address ){
    if ( !FUNC_DYNAMIC_INFO_STACK.back()->skip ){
        assert( FUNC_DYNAMIC_INFO_STACK.back()->address == rtn_address );
        // most basic filter: uniqueness determined based on the function inputs
        FUNC_DYNAMIC_INFO_STACK.back()->unique_id = &(FUNC_DYNAMIC_INFO_STACK.back()->io_var_bytes);
    }
}

void process_io_var( ADDRINT rtn_address, ADDRINT val_base_ptr, ADDRINT var_idx ){

    if ( !FUNC_DYNAMIC_INFO_STACK.back()->skip ){

        // get n_values
        FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_n_values[var_idx] = get_n_values(var_idx, FUNC_DYNAMIC_INFO_STACK.back(), FUNC_STATIC_INFO_MAP[rtn_address]);

        // added this branch because BLAS regression tests revealed a case in which bad parameters are provided to the function
        // which give non-positive n_values to test error handling. We skip such executions.
        if ( FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_n_values[var_idx] == 0 ){
            FUNC_DYNAMIC_INFO_STACK.back()->skip = true;
            return;
        }

        // save value for use in evaluating the n_value_strings of other io_vars later in the determined process_order
        if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type == INTEGER ){
            FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_value[var_idx] = *(reinterpret_cast<int*>(val_base_ptr));
        }
        else if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type == CHARACTER ){
            FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_value[var_idx] = (int)(*(reinterpret_cast<char*>(val_base_ptr)));
        }
        else{
            FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_value[var_idx] = 0;
        }

        // read all of the value's bytes
        if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type != REAL_RETURN ){
            uint32_t total_bytes = FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].n_bytes * FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_n_values[var_idx];
            for ( uint32_t i=0; i < total_bytes; ++i ){
                FUNC_DYNAMIC_INFO_STACK.back()->io_var_bytes.push_back(*(reinterpret_cast<uint8_t*>(val_base_ptr + i)));
            }
#ifdef IO_VAR_MAX_INPUT_FLOATS
            uint32_t n_input_floats = 0;
            for ( uint32_t i = 0; i < FUNC_STATIC_INFO_MAP[rtn_address]->io_vars.size(); ++i ){
                if ( (FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[i].type == REAL_IN) || (FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[i].type == REAL_INOUT) ){
                    n_input_floats = n_input_floats + FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_n_values[i];
                }
            }
            if ( n_input_floats > IO_VAR_MAX_INPUT_FLOATS ){
                FUNC_DYNAMIC_INFO_STACK.back()->skip = true;
                return;
            }
#endif 
#if VERBOSE >= 1
            uint32_t offset = 0;
            ostringstream oss;
            oss << FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].name << ": ";
            for ( uint32_t i=0; i < FUNC_DYNAMIC_INFO_STACK.back()->io_var_idx_to_n_values[var_idx]; ++i ){
                if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type == INTEGER ){
                    oss << *(reinterpret_cast<int*>(val_base_ptr + offset)) << " ";
                }
                else if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type == CHARACTER ){
                    oss << *(reinterpret_cast<char*>(val_base_ptr + offset)) << " ";
                }
                else if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].type >= REAL_IN ){
                    if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].n_bytes == 4 ){
                        oss << *(reinterpret_cast<float*>(val_base_ptr + offset)) << " ";
                    }
                    else if ( FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].n_bytes == 8 ){
                        oss << *(reinterpret_cast<double*>(val_base_ptr + offset)) << " ";
                    } 
                }
                offset = offset + FUNC_STATIC_INFO_MAP[rtn_address]->io_vars[var_idx].n_bytes;
            }
            log1(oss.str());
#endif
        }
    }   
}

void post_call( ADDRINT ip ){
#ifdef RESTRICTIVE
        for ( const auto& func_dynamic_info : FUNC_DYNAMIC_INFO_STACK ){
            for ( ADDRINT address : FUNC_DYNAMIC_INFO_STACK.back()->unique_addresses ){
                func_dynamic_info->unique_addresses.insert(address);
            }
        }

        for ( ADDRINT address : FUNC_DYNAMIC_INFO_STACK.back()->unique_addresses ){
            for ( int i = 0; i < 8; ++i ){
                FUNC_DYNAMIC_INFO_STACK.back()->unique_id->push_back(static_cast<uint8_t>((address >> (i*8)) & 0xFF));
            }
        }
#endif

    if ( FUNC_DYNAMIC_INFO_STACK.back()->skip ){
        ++EXECUTION_FILTER_COUNT;
    }
    else{

        // if this is a new unique id, save this execution
        size_t hash = BYTE_HASH(*(FUNC_DYNAMIC_INFO_STACK.back()->unique_id));
        if ( SAVED_IO_VARS_MAPS[FUNC_DYNAMIC_INFO_STACK.back()->address].find(hash) == SAVED_IO_VARS_MAPS[FUNC_DYNAMIC_INFO_STACK.back()->address].end() ){
            SAVED_IO_VARS_MAPS[FUNC_DYNAMIC_INFO_STACK.back()->address][hash] = FUNC_DYNAMIC_INFO_STACK.back()->io_var_bytes;
        }
        else{

            // otherwise, save the smaller one as it will be easier for the SMT solver later
            if ( SAVED_IO_VARS_MAPS[FUNC_DYNAMIC_INFO_STACK.back()->address][hash].size() > FUNC_DYNAMIC_INFO_STACK.back()->io_var_bytes.size() ){
                SAVED_IO_VARS_MAPS[FUNC_DYNAMIC_INFO_STACK.back()->address][hash] = FUNC_DYNAMIC_INFO_STACK.back()->io_var_bytes;
            }
            ++EXECUTION_FILTER_COUNT;
        }
    }

#ifdef RESTRICTIVE
        free(FUNC_DYNAMIC_INFO_STACK.back()->unique_id);
#endif
    delete FUNC_DYNAMIC_INFO_STACK.back();
    FUNC_DYNAMIC_INFO_STACK.pop_back();
}

ADDRINT if_target_of_return( ADDRINT ip ){
    return FUNC_DYNAMIC_INFO_STACK.back()->return_ip == ip;
}

void log_fork_in_control_flow( bool branch_taken, ADDRINT taken_address, ADDRINT fallthrough_address ){
    if ( branch_taken ){
        FUNC_DYNAMIC_INFO_STACK.back()->unique_addresses.insert(taken_address);
    }
    else{
        FUNC_DYNAMIC_INFO_STACK.back()->unique_addresses.insert(fallthrough_address);
    }
}

void log_x87_ins(){
    if ( FUNC_DYNAMIC_INFO_STACK.size() > 1 ){
        x87_INSTRUCTIONS_EXECUTED.insert(FUNC_STATIC_INFO_MAP[FUNC_DYNAMIC_INFO_STACK.back()->address]->name);
    }
}

void log_unsupported_instruction(){
    if ( FUNC_DYNAMIC_INFO_STACK.size() > 1 ){
        UNSUPPORTED_INSTRUCTIONS.insert(FUNC_STATIC_INFO_MAP[FUNC_DYNAMIC_INFO_STACK.back()->address]->name);
    }
}

void image_load2( IMG img, VOID *v ){
    // iterate through sections in this image
    for ( SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec) ){

        // if this section is executable, process any routines therein
        if ( SEC_IsExecutable(sec) ){
            for ( RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn) ){
                RTN_Open(rtn);
                for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)){
#if VERBOSE >= 2
                    if ( false ){
                        ROUTINE_NAME_MAP[RTN_Id(rtn)] = RTN_Name(rtn);
                        Instruction* ins_obj_ptr = construct_instruction_object(ins);
                        ADDRINT ins_offset = INS_Address(ins) - RTN_Address(INS_Rtn(ins));
                        INSTRUCTION_OBJ_MAP[RTN_Id(rtn)][ins_offset] = ins_obj_ptr; 
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)print_instruction, IARG_ADDRINT, RTN_Id(rtn), IARG_ADDRINT, ins_offset, IARG_ADDRINT, INS_Opcode(ins), IARG_END);
                    }
#endif
                    for ( uint32_t i = 0; i < INS_OperandCount(ins); ++i ){
                        REG reg = INS_OperandReg(ins, i);
                        if ( REG_is_st(reg) ){
                            INS_InsertCall(
                                ins,
                                IPOINT_BEFORE,
                                (AFUNPTR) log_x87_ins,
                                IARG_END
                            );
                            break;
                        }
                    }

                    if ( opcode_to_smtlib2(INS_Opcode(ins)).find("ERROR:") != string::npos ){
                        INS_InsertCall(
                            ins,
                            IPOINT_BEFORE,
                            (AFUNPTR) log_unsupported_instruction,
                            IARG_END
                        );
                    }

                    INS_InsertIfCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR) if_target_of_return,
                        IARG_REG_VALUE, REG_INST_PTR,
                        IARG_END
                    );
                    INS_InsertThenCall(
                        ins,
                        IPOINT_BEFORE,
                        (AFUNPTR) post_call,
                        IARG_REG_VALUE, REG_INST_PTR,
                        IARG_END
                    );

                    // ADDRINT rtn_address = RTN_Address(rtn);
                    // ADDRINT ins_offset = INS_Address(ins) - rtn_address;
                    // if ( INS_EXECUTION_COUNTS.find(STRING_HASH(to_string(rtn_address) + to_string(ins_offset))) == INS_EXECUTION_COUNTS.end() ){
                    //     INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))] = 0;
                    // }
                    // INS_InsertIfCall(
                    //     ins,
                    //     IPOINT_BEFORE,
                    //     (AFUNPTR) if_go,
                    //     IARG_END
                    // );
                    // INS_InsertThenCall(
                    //     ins,
                    //     IPOINT_BEFORE,
                    //     (AFUNPTR) ins_count,
                    //     IARG_ADDRINT, rtn_address,
                    //     IARG_ADDRINT, ins_offset,
                    //     IARG_END
                    // );
                    // if ( is_EV_generator_reg(INS_Opcode(ins)) > NOT_INJECTABLE && is_EV_generator_reg(INS_Opcode(ins)) < ADDSUB ){        
                    //     if ( EVABLE_INS_EXECUTION_COUNTS.find(STRING_HASH(to_string(rtn_address) + to_string(ins_offset))) == EVABLE_INS_EXECUTION_COUNTS.end() ){
                    //         EVABLE_INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))] = 0;
                    //     }
                    //     INS_InsertIfCall(
                    //         ins,
                    //         IPOINT_BEFORE,
                    //         (AFUNPTR) if_go,
                    //         IARG_END
                    //     );
                    //     INS_InsertThenCall(
                    //         ins,
                    //         IPOINT_BEFORE,
                    //         (AFUNPTR) EVable_ins_count,
                    //         IARG_ADDRINT, rtn_address,
                    //         IARG_ADDRINT, ins_offset,
                    //         IARG_END
                    //     );
                    // }
                    // else if ( is_EV_generator_reg(INS_Opcode(ins)) >= ADDSUB ){        
                    //     if ( NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS.find(STRING_HASH(to_string(rtn_address) + to_string(ins_offset))) == NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS.end() ){
                    //         NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))] = 0;
                    //     }
                    //     INS_InsertIfCall(
                    //         ins,
                    //         IPOINT_BEFORE,
                    //         (AFUNPTR) if_go,
                    //         IARG_END
                    //     );
                    //     INS_InsertThenCall(
                    //         ins,
                    //         IPOINT_BEFORE,
                    //         (AFUNPTR) non_targeted_EVable_ins_count,
                    //         IARG_ADDRINT, rtn_address,
                    //         IARG_ADDRINT, ins_offset,
                    //         IARG_END
                    //     );
                    // }

#ifdef RESTRICTIVE
                    // restrictive setting
                    if ( INS_IsControlFlow(ins) && INS_HasFallThrough(ins) ){
                        INS_InsertIfCall(
                            ins,
                            IPOINT_BEFORE,
                            (AFUNPTR) if_go,
                            IARG_END
                        );
                        INS_InsertThenCall(
                            ins,
                            IPOINT_BEFORE,
                            (AFUNPTR) log_fork_in_control_flow,
                            IARG_BRANCH_TAKEN,
                            IARG_BRANCH_TARGET_ADDR,
                            IARG_FALLTHROUGH_ADDR,
                            IARG_END
                        );
                    }
#endif
                }
                RTN_Close(rtn);
            }
        }
    }
}

void image_load1( IMG img, VOID *v ){

    ifstream in_file;
    in_file.open("__EXCVATE/__input_file_list");
    string file_path;
    while ( getline(in_file, file_path) ){
        if (file_path == ""){
            break;
        }

        FuncStaticInfo* func_static_info = process_prototype_file(file_path);

        RTN rtn = RTN_FindByName(img, func_static_info->name.c_str());
        if ( RTN_Valid(rtn) ){
            RTN_Open(rtn);

            // save address and mapping of address to prototype
            func_static_info->address = RTN_Address(rtn);
            FUNC_STATIC_INFO_MAP[func_static_info->address] = func_static_info;

            string saved_io_var_map_name = "__EXCVATE/" + func_static_info->name + "/" + func_static_info->name + ".io_vars";
            load_io_var_map( SAVED_IO_VARS_MAPS[func_static_info->address], saved_io_var_map_name );

            REGSET regset_in;
            REGSET_Clear(regset_in);
            REGSET_Insert(regset_in, REG_MXCSR);
            REGSET regset_out;
            REGSET_Clear(regset_out);
            RTN_InsertCall(
                rtn,
                IPOINT_BEFORE,
                (AFUNPTR) pre_call,
                IARG_ADDRINT, func_static_info->address,
                IARG_RETURN_IP,
                IARG_PARTIAL_CONTEXT, &regset_in, &regset_out,
                IARG_END
            );

            for ( const auto& i : func_static_info->process_order ){
                RTN_InsertCall(
                    rtn,
                    IPOINT_BEFORE,
                    (AFUNPTR) process_io_var,
                    IARG_ADDRINT, func_static_info->address,
                    IARG_FUNCARG_ENTRYPOINT_VALUE, i,
                    IARG_ADDRINT, i,
                    IARG_END
                );
            }

#ifndef RESTRICTIVE
            RTN_InsertCall(
                rtn,
                IPOINT_BEFORE,
                (AFUNPTR) basic_filter,
                IARG_ADDRINT, func_static_info->address,
                IARG_END
            );
#endif
            RTN_Close(rtn);
        }
    }
}

ADDRINT if_go(){
    return FUNC_DYNAMIC_INFO_STACK.size() > 0;
}

void ins_count( ADDRINT rtn_address, ADDRINT ins_offset ){
    INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))]++;
}

void EVable_ins_count( ADDRINT rtn_address, ADDRINT ins_offset ){
    EVABLE_INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))]++;
}

void non_targeted_EVable_ins_count( ADDRINT rtn_address, ADDRINT ins_offset ){
    NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS[STRING_HASH(to_string(rtn_address) + to_string(ins_offset))]++;
}

void Fini( int32_t code, void *v ){

    // save_execution_count_map_to_file(INS_EXECUTION_COUNTS, "__EXCVATE/ins_execution_counts.map");
    // save_execution_count_map_to_file(EVABLE_INS_EXECUTION_COUNTS, "__EXCVATE/EVable_ins_execution_counts.map");
    // save_execution_count_map_to_file(NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS, "__EXCVATE/non_targeted_EVable_ins_execution_counts.map");

    // uint64_t executed_count = 0;
    // for ( const auto &x : INS_EXECUTION_COUNTS ){
    //     if ( x.second > 0 ){
    //         ++executed_count;
    //     }
    // }
    // cout << "                         Instruction Coverage: " << executed_count << "/" << INS_EXECUTION_COUNTS.size() << " (" << fixed << setprecision(4) << 100 * ((double) executed_count / (double) INS_EXECUTION_COUNTS.size()) << "%)" << endl;

    // executed_count = 0;
    // for ( const auto &x : EVABLE_INS_EXECUTION_COUNTS ){
    //     if ( x.second > 0 ){
    //         ++executed_count;
    //     }
    // }
    // cout << "                 EV-able Instruction Coverage: " << executed_count << "/" << EVABLE_INS_EXECUTION_COUNTS.size() << " (" << fixed << setprecision(4) << 100 * ((double) executed_count / (double) EVABLE_INS_EXECUTION_COUNTS.size()) << "%)" << endl;

    // executed_count = 0;
    // for ( const auto &x : NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS ){
    //     if ( x.second > 0 ){
    //         ++executed_count;
    //     }
    // }
    // cout << "    Non-targeted EV-able Instruction Coverage: " << executed_count << "/" << NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS.size() << " (" << fixed << setprecision(4) << 100 * ((double) executed_count / (double) NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS.size()) << "%)" << endl;

    // cout << endl;
    uint64_t saved_executions = 0;
    ostringstream out_file_name;
    cout << endl;
    for ( const auto &x : SAVED_IO_VARS_MAPS ){
        cout << "    saved " << x.second.size() << " " << FUNC_STATIC_INFO_MAP[x.first]->name << " executions for replay"<< endl;
        saved_executions = saved_executions + x.second.size();
        string out_file_name = "__EXCVATE/" + FUNC_STATIC_INFO_MAP[x.first]->name + "/" + FUNC_STATIC_INFO_MAP[x.first]->name + ".io_vars";
        save_io_var_map(x.second, out_file_name);
    }

    cout << "    saved " << saved_executions << "/" << saved_executions + EXECUTION_FILTER_COUNT << " executions for replay"<< endl;

    for ( const auto &x : FUNC_STATIC_INFO_MAP ){
        delete x.second;
    }

    cout << endl;
    for ( const auto &x : x87_INSTRUCTIONS_EXECUTED ){
        cout << "    (WARNING) unsupported instructions executed in " << x << " (x87 instructions)" << endl;
    }
    for ( const auto &x : UNSUPPORTED_INSTRUCTIONS ){
        cout << "    (WARNING) unsupported instructions executed in " << x << endl;
    }
}

void save_execution_count_map_to_file(const map<size_t, uint64_t>& execution_count_map, const string& file_name) {
    ofstream out_file(file_name, ios::binary);
    if (out_file) {

        // Write the size of the map first
        size_t size = execution_count_map.size();
        out_file.write(reinterpret_cast<const char*>(&size), sizeof(size));

        // Write each key-value pair
        for (const auto& [key, value] : execution_count_map) {
            out_file.write(reinterpret_cast<const char*>(&key), sizeof(key));
            out_file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        out_file.close();
    }
}

void load_execution_count_map_from_file(map<size_t, uint64_t>& execution_count_map, const string& file_name) {
    ifstream in_file(file_name, ios::binary);
    if (in_file) {

        // Read the size of the map
        size_t size;
        in_file.read(reinterpret_cast<char*>(&size), sizeof(size));

        for (size_t i = 0; i < size; ++i) {
            size_t key;
            uint64_t value;
            in_file.read(reinterpret_cast<char*>(&key), sizeof(key));
            in_file.read(reinterpret_cast<char*>(&value), sizeof(value));
            execution_count_map[key] = value;
        }

        in_file.close();
    }
}

// Entry point for the tool
int main( int argc, char *argv[] ){

    // Initialize Pin
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)){
        std::cerr << "Error during PIN_Init" << std::endl;
        return 1;
    }

    // load_execution_count_map_from_file(INS_EXECUTION_COUNTS, "__EXCVATE/ins_execution_counts.map");
    // load_execution_count_map_from_file(EVABLE_INS_EXECUTION_COUNTS, "__EXCVATE/EVable_ins_execution_counts.map");
    // load_execution_count_map_from_file(NON_TARGETED_EVABLE_INS_EXECUTION_COUNTS, "__EXCVATE/non_targeted_EVable_ins_execution_counts.map");

    // Set up instrumentation functions
    IMG_AddInstrumentFunction(image_load1, NULL);
    IMG_AddInstrumentFunction(image_load2, NULL);
    PIN_AddFiniFunction(Fini, NULL);

    FuncDynamicInfo* dummy_info = new FuncDynamicInfo;
    dummy_info->skip = true;
    FUNC_DYNAMIC_INFO_STACK.push_back(dummy_info);

    // Start the application
    PIN_StartProgram();

    return 0;
}