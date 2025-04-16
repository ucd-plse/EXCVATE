#include "pin.H"
#include <sstream>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>

#define VERBOSE 0

enum VarType {
    INTEGER = -2,
    CHARACTER = -1,
    OTHER = 0,
    REAL_IN = 1,
    REAL_OUT = 2,
    REAL_INOUT = 3,
    REAL_RETURN = 4,
};

enum InjectableType {
    NOT_INJECTABLE = 0,
    ADD = 1,
    SUB = 2,
    MUL = 3,
    DIV = 4,
    SQRT = 5,
    ADDSUB = 6,
    FMA = 7,
};

struct Operand {
    REG reg;
    ADDRINT imm_value;
    uint32_t n_values;
    uint32_t n_bytes;
    uint32_t memory_operand_idx;
};

struct Instruction {
    ADDRINT opcode;
    std::string disassembly = "";
    std::string src_location = "";
    std::string smtlib2_op = "";
    std::vector<Operand*> write_operands;
    std::vector<Operand*> read_operands;
    std::vector<Operand*> write_vec_registers;
    std::vector<Operand*> read_vec_registers;
    std::vector<Operand*> write_memory;
    std::vector<Operand*> read_memory;
    std::vector<Operand*> immediates;
};

struct VarProperties {
    std::string name;
    VarType type;
    uint32_t n_bytes;
    std::string n_values_string;
};

struct FuncStaticInfo {
    std::string name;
    ADDRINT address;
    std::vector<VarProperties> io_vars;
    std::map<std::string,uint32_t> var_name_to_idx;
    std::vector<uint32_t> process_order;
};

struct FuncDynamicInfo {
    ADDRINT address;
    ADDRINT return_ip;
    bool skip = false;
    std::vector<uint32_t> io_var_idx_to_n_values;
    std::vector<int32_t> io_var_idx_to_value;
    std::vector<uint8_t> io_var_bytes;
    std::vector<uint8_t>* unique_id = NULL;
    std::set<ADDRINT> unique_addresses;
};

void log0( std::string message );
void log1( std::string message );
void log2( std::string message );
void log3( std::string message );
FuncStaticInfo* process_prototype_file( std::string line );
std::string replace_symbols_and_remove_whitespace( std::string n_values_string, FuncDynamicInfo* func_dynamic_info, FuncStaticInfo* func_static_info );
bool evaluate_conditional( std::string conditional_expression );
int32_t evaluate_expression( std::string in_string );
int32_t evaluate_expression_helper( std::string expression );
std::string parse_if_statement( std::string n_values_string );
uint32_t get_n_values( ADDRINT var_idx, FuncDynamicInfo* func_dynamic_info, FuncStaticInfo* func_static_info );
uint32_t load_io_var_map( std::map<const size_t, std::vector<uint8_t>>& map, const std::string& filename );
void save_io_var_map( const std::map<const size_t, std::vector<uint8_t>>& map, const std::string& filename );
bool is_vector_reg( REG reg );
std::string get_source_location( ADDRINT address );
bool is_bitwise_and( OPCODE iclass );
bool is_non_EVable( OPCODE iclass );
bool ignore_instruction( OPCODE iclass );
bool is_minmax( OPCODE iclass );
InjectableType is_EV_generator_reg( OPCODE iclass );

void log0( std::string message ){
    LOG("(!!) " + message + "\n");
}

void log1( std::string message ){
#if VERBOSE >= 1
    LOG(message + "\n");
#endif
}

void log2( std::string message ){
#if VERBOSE >= 2
    LOG("\t\t%% " + message + "\n");
#endif
}

void log3( std::string message ){
#if VERBOSE >= 3
    LOG("\t\t\t%% " + message + "\n");
#endif
}

Instruction* construct_instruction_object(INS ins){

    Instruction* ins_obj_ptr = new Instruction;
    ins_obj_ptr->opcode = INS_Opcode(ins);
    ins_obj_ptr->disassembly = INS_Disassemble(ins);
    ins_obj_ptr->src_location = get_source_location(INS_Address(ins));

    // note vector register and memory location operands
    uint32_t memory_operand_idx = 0;
    for ( uint32_t i = 0; i < INS_OperandCount(ins); ++i ){

        Operand* op_obj_ptr = new Operand;
        op_obj_ptr->n_values = INS_OperandElementCount(ins, i);
        op_obj_ptr->n_bytes = INS_OperandElementSize(ins, i);

        REG reg = INS_OperandReg(ins, i);
        if ( is_vector_reg(reg) ){
            op_obj_ptr->reg = reg;
            if ( INS_OperandWritten(ins, i) ){
                ins_obj_ptr->write_operands.push_back(op_obj_ptr);
                ins_obj_ptr->write_vec_registers.push_back(op_obj_ptr);
            }
            if ( INS_OperandRead(ins, i) ){
                ins_obj_ptr->read_operands.push_back(op_obj_ptr);
                ins_obj_ptr->read_vec_registers.push_back(op_obj_ptr);
            }
        }
        else if ( INS_OperandIsMemory(ins, i) ){
            op_obj_ptr->reg = REG_INVALID();
            op_obj_ptr->memory_operand_idx = memory_operand_idx++;
            if ( INS_OperandWritten(ins, i) ){
                ins_obj_ptr->write_operands.push_back(op_obj_ptr);
                ins_obj_ptr->write_memory.push_back(op_obj_ptr);
            }
            if ( INS_OperandRead(ins, i) ){
                ins_obj_ptr->read_operands.push_back(op_obj_ptr);
                ins_obj_ptr->read_memory.push_back(op_obj_ptr);
            }
        }
        else if ( INS_OperandIsImmediate(ins, i) ){
            op_obj_ptr->imm_value = INS_OperandImmediate(ins, i);
            ins_obj_ptr->immediates.push_back(op_obj_ptr);
        }
    }

    // spotfix for UNPCKLPS
    if ( (INS_Opcode(ins) == XED_ICLASS_UNPCKLPS) && (ins_obj_ptr->read_vec_registers.size() == 2) ){
        ins_obj_ptr->read_vec_registers.back()->n_values = 2 * ins_obj_ptr->read_vec_registers.back()->n_values;
        ins_obj_ptr->read_vec_registers.back()->n_bytes = ins_obj_ptr->read_vec_registers.back()->n_bytes / 2;
    }

    return ins_obj_ptr;
}

FuncStaticInfo* process_prototype_file( std::string line ){
    FuncStaticInfo* func_static_info = new FuncStaticInfo;

    std::istringstream iss(line);
    std::string prototype_file_path;
    iss >> prototype_file_path;

    // extract name from prototype file name
    func_static_info->name = prototype_file_path.substr(0, prototype_file_path.find_last_of("."));
    if ( func_static_info->name.find("/") != std::string::npos ){
        func_static_info->name = func_static_info->name.substr(prototype_file_path.find_last_of('/') + 1, std::string::npos);
    }

    // read io_var properties from prototype file
    std::ifstream in_file;
    in_file.open(prototype_file_path.c_str());
    if ( in_file.is_open() ){

        std::string line_buffer;
        while ( getline(in_file, line_buffer) ){
            if (line_buffer == ""){
                break;
            }

            VarProperties v;
            std::string type_buffer;
            std::string intent_buffer;
            std::istringstream iss(line_buffer);

            iss >> v.name;
            iss >> type_buffer;

            int n_bits;
            iss >> n_bits;
            v.n_bytes = n_bits/8;

            iss >> intent_buffer;
            getline(iss, v.n_values_string);
            if ( type_buffer == "i" ){
                v.type = INTEGER;
            }
            else if ( type_buffer == "c" ){
                v.type = CHARACTER;
            }
            else if ( type_buffer == "r" ){
                if ( intent_buffer == "in" ){
                    v.type = REAL_IN;
                }
                else if ( intent_buffer == "out" ){
                    v.type = REAL_OUT;
                }
                else if ( intent_buffer == "inout" ){
                    v.type = REAL_INOUT;
                }
                else if ( intent_buffer == "return"){
                    v.type = REAL_RETURN;
                }
                else{
                    assert(false && "unsupported intent in prototype file");
                }
            }
            func_static_info->var_name_to_idx[v.name] = func_static_info->io_vars.size();
            func_static_info->io_vars.push_back(v);
        }
        in_file.close();
    }
    else{
        assert(false && "unable to open prototype file for reading");
    }

    // deduce process order based on dependencies between n_values_string attributes
    while ( func_static_info->process_order.size() < func_static_info->io_vars.size() ){
        for ( long unsigned int i = 0; i < func_static_info->io_vars.size(); ++i ){

            // skip io_vars that have already been processed
            auto it = find(func_static_info->process_order.begin(), func_static_info->process_order.end(), i);
            if ( it != func_static_info->process_order.end() ){
                continue;
            }

            // add io_vars that do not reference others
            if ( func_static_info->io_vars[i].n_values_string.find("$") == std::string::npos ){
                func_static_info->process_order.push_back(i);
            }
            else{
                
                // otherwise, we add only those io_vars whose dependencies have already been added
                std::istringstream iss(func_static_info->io_vars[i].n_values_string);
                char char_buffer;
                int depend_val_idx;
                bool depends_satisfied = true;
                while ( iss >> char_buffer ){
                    if ( char_buffer == '$' ){
                        iss >> char_buffer; // for the { char
                        iss >> char_buffer; // first char of the var name
                        std::ostringstream var_name;
                        while ( char_buffer != '}' ){
                            var_name << char_buffer;
                            iss >> char_buffer;
                        }
                        depend_val_idx = func_static_info->var_name_to_idx[var_name.str()];

                        it = find(func_static_info->process_order.begin(), func_static_info->process_order.end(), depend_val_idx);
                        if ( it == func_static_info->process_order.end() ){
                            depends_satisfied = false;
                            break;
                        }
                    }
                }
                if ( depends_satisfied ){
                    func_static_info->process_order.push_back(i);
                }
            }
        }
    }

    return func_static_info;
}

std::string replace_symbols_and_remove_whitespace( std::string n_values_string, FuncDynamicInfo* func_dynamic_info, FuncStaticInfo* func_static_info ){

    std::ostringstream oss;
    std::istringstream iss(n_values_string);

    char char_buffer;
    int depend_val_idx;
    while ( iss >> char_buffer ){
        if ( char_buffer == '$' ){
            iss >> char_buffer; // for the { char
            iss >> char_buffer; // first char of the var name
            std::ostringstream var_name;
            while ( char_buffer != '}' ){
                var_name << char_buffer;
                iss >> char_buffer;
            }
            depend_val_idx = func_static_info->var_name_to_idx[var_name.str()];
            if ( func_static_info->io_vars[depend_val_idx].type == INTEGER ){
                oss << func_dynamic_info->io_var_idx_to_value[depend_val_idx];
            }
            else if ( func_static_info->io_vars[depend_val_idx].type == CHARACTER ){
                oss << static_cast<char>(func_dynamic_info->io_var_idx_to_value[depend_val_idx]);
            }
            else{
                assert(false && "io_var substitution not an integer or character");
            }
        }
        else{
            oss << char_buffer;
        }
    }
    return oss.str();
}

// note that comparisons involving character literals are case-insensitive
bool evaluate_conditional( std::string conditional_expression ){
    std::istringstream iss(conditional_expression);
    std::string lhs;
    std::string rhs;
    std::string op;
    char char_buffer;
    while ( op == "" ){
        iss >> char_buffer;
        if ( isalpha(char_buffer) ){
            char_buffer = toupper(char_buffer);
        }

        switch ( char_buffer ){
            case ' ':
                continue;
            case '=':
                op.push_back(char_buffer);
                break;
            case '!':
                op.push_back(char_buffer);
                break;
            case '>':
                op.push_back(char_buffer);
                break;
            case '<':
                op.push_back(char_buffer);
                break;
            default:
                lhs.push_back(char_buffer);
        }
    }

    while ( rhs == "" ){
        iss >> char_buffer;
        if ( isalpha(char_buffer) ){
            char_buffer = toupper(char_buffer);
        }

        switch ( char_buffer ){
            case ' ':
                continue;
            case '=':
                op.push_back(char_buffer);
                break;
            case '!':
                op.push_back(char_buffer);
                break;
            case '>':
                op.push_back(char_buffer);
                break;
            case '<':
                op.push_back(char_buffer);
                break;
            default:
                rhs.push_back(char_buffer);
        }
    }

    while ( iss >> char_buffer ){
        if ( isalpha(char_buffer) ){
            char_buffer = toupper(char_buffer);
        }

        switch ( char_buffer ){
            case ' ':
                continue;
            default:
                rhs.push_back(char_buffer);
        }
    }

    if ( op == "==" ){
        return lhs == rhs;
    }
    else if ( op == "!=" ){
        return lhs != rhs;
    }
    else{
        int lhs_int;
        iss.str(lhs);
        iss >> lhs_int;
        int rhs_int;
        iss.str(rhs);
        iss >> rhs_int;
        if ( op == ">" ){
            return lhs_int > rhs_int;
        }
        else if ( op == ">=" ){
            return lhs_int >= rhs_int;
        }
        else if ( op == "<" ){
            return lhs_int < rhs_int;
        }
        else if ( op == "<=" ){
            return lhs_int <= rhs_int;
        }
        else{
            assert(false && "unexpected boolean operator");
        }
    }
}

int32_t evaluate_expression( std::string in_string ){

    std::istringstream iss(in_string);
    std::string expression;
    char char_buffer;
    while ( iss >> char_buffer ){
        expression.push_back(char_buffer);

        if ( expression.find("abs") != std::string::npos && char_buffer == '(' ){
            uint32_t unmatched_paren_count = 1;
            std::string subexpression = "(";

            while ( unmatched_paren_count > 0 ){
                iss >> char_buffer;
                expression.push_back(char_buffer);
                subexpression.push_back(char_buffer);

                if ( char_buffer == '(' ){
                    ++unmatched_paren_count;
                }
                else if ( char_buffer == ')' ){
                    --unmatched_paren_count;
                }
            }
            std::ostringstream oss;
            oss << abs(evaluate_expression(subexpression));
            expression = expression.substr(0, expression.find("abs")) + oss.str();
        }
    }

    return evaluate_expression_helper(expression);
}

std::string parse_if_statement( std::string n_values_string ){

    std::string conditional_expression = n_values_string.substr(n_values_string.find("[") + 1, std::string::npos );
    conditional_expression = conditional_expression.substr(0, conditional_expression.find("]"));
    n_values_string = n_values_string.substr(n_values_string.find(";"), std::string::npos );

    if ( evaluate_conditional(conditional_expression) ){
        n_values_string = n_values_string.substr(n_values_string.find("then") + 4, std::string::npos );
        n_values_string = n_values_string.substr(0, n_values_string.find(";"));
    }
    else{
        n_values_string = n_values_string.substr(n_values_string.find("else") + 4, std::string::npos );
        n_values_string = n_values_string.substr(0, n_values_string.find_last_of(";"));
    }

    return n_values_string;
}

uint32_t get_n_values( ADDRINT var_idx, FuncDynamicInfo* func_dynamic_info, FuncStaticInfo* func_static_info ){
    std::string n_values_string = replace_symbols_and_remove_whitespace(func_static_info->io_vars[var_idx].n_values_string, func_dynamic_info, func_static_info);
    while ( n_values_string.find("if") == 0 ){
        n_values_string = parse_if_statement(n_values_string);
    }

    int32_t n_values = evaluate_expression(n_values_string);

    if ( n_values <= 0){
        return 0;
    }
    else{
        return (uint32_t) n_values;
    }
}

uint32_t load_io_var_map( std::map<const size_t, std::vector<uint8_t>>& map, const std::string& filename ){
    
    uint32_t max_io_var_bytes = 0;

    std::ifstream in_file(filename, std::ios::binary);
    if (!in_file){
        return max_io_var_bytes;
    }

    // Read the size of the map
    size_t map_size;
    in_file.read(reinterpret_cast<char*>(&map_size), sizeof(size_t));

    // Read the map elements
    for (size_t i = 0; i < map_size; ++i) {

        // Read key
        size_t key;
        in_file.read(reinterpret_cast<char*>(&key), sizeof(size_t));

        // Read value
        size_t n_bytes;
        in_file.read(reinterpret_cast<char*>(&n_bytes), sizeof(size_t));
        if ( n_bytes > max_io_var_bytes ){
            max_io_var_bytes = n_bytes;
        }
        std::vector<uint8_t> values(n_bytes);
        in_file.read(reinterpret_cast<char*>(values.data()), n_bytes * sizeof(uint8_t));

        map[key] = values;
    }

    in_file.close();
    return max_io_var_bytes;
}

void save_io_var_map( const std::map<const size_t, std::vector<uint8_t>>& map, const std::string& filename ){
    std::ofstream outfile(filename, std::ios::binary);

    // Write the size of the map
    size_t map_size = map.size();
    outfile.write(reinterpret_cast<const char*>(&map_size), sizeof(size_t));

    // Write the map elements
    for (const auto& pair : map) {
        
        // Write key
        outfile.write(reinterpret_cast<const char*>(&pair.first), sizeof(size_t));

        // Write value
        size_t n_bytes = pair.second.size();
        outfile.write(reinterpret_cast<const char*>(&n_bytes), sizeof(size_t));
        outfile.write(reinterpret_cast<const char*>(pair.second.data()), n_bytes * sizeof(uint8_t));
    }

    outfile.close();
}


bool is_vector_reg( REG reg ){
    if (reg == REG_INVALID()){return false;}
    else if ( REG_is_xmm(reg) ){return true;}
    else if ( REG_is_ymm(reg) ){return true;}
#ifdef AVX512
    else if ( REG_is_zmm(reg) ){return true;}
#endif
    else{
        return false;
    }
}

std::string get_source_location( ADDRINT address ){

    int column_n;
    int line_n;
    std::string file_name;
    PIN_GetSourceLocation(address, &column_n, &line_n, &file_name);

    std::ostringstream oss;
    if (!file_name.empty() && line_n != 0){
        oss << file_name;
        oss << ":";
        oss << line_n;
    }
    return oss.str();
}

bool is_bitwise_and( OPCODE iclass ){

    switch (iclass){
        case XED_ICLASS_AND:
            return true;
        case XED_ICLASS_ANDN:
            return true;
        case XED_ICLASS_ANDNPD:
            return true;
        case XED_ICLASS_ANDNPS:
            return true;
        case XED_ICLASS_ANDPD:
            return true;
        case XED_ICLASS_ANDPS:
            return true;
        case XED_ICLASS_AND_LOCK:
            return true;
    }
    return false;
}


bool is_non_EVable( OPCODE iclass ){

    switch (iclass){
        case XED_ICLASS_PSRLW:
            return true;
        case XED_ICLASS_PSRLD:
            return true;
        case XED_ICLASS_PSRLQ:
            return true;
        case XED_ICLASS_VPSRLW:
            return true;
        case XED_ICLASS_VPSRLD:
            return true;
        case XED_ICLASS_VPSRLQ:
            return true;
        case XED_ICLASS_PSLLW:
            return true;
        case XED_ICLASS_PSLLD:
            return true;
        case XED_ICLASS_PSLLQ:
            return true;
        case XED_ICLASS_VPSLLW:
            return true;
        case XED_ICLASS_VPSLLD:
            return true;
        case XED_ICLASS_VPSLLQ:
            return true;
        case XED_ICLASS_PSRAW:
            return true;
        case XED_ICLASS_PSRAD:
            return true;
        case XED_ICLASS_VPSRAW:
            return true;
        case XED_ICLASS_VPSRAD:
            return true;
        case XED_ICLASS_VPSRAQ:
            return true;
        case XED_ICLASS_PSLLDQ:
            return true;
        case XED_ICLASS_PSRLDQ:
            return true;
        case XED_ICLASS_VPSLLDQ:
            return true;
        case XED_ICLASS_VPSRLDQ:
            return true;
        case XED_ICLASS_VPXOR:
            return true;
        case XED_ICLASS_VPXORD:
            return true;
        case XED_ICLASS_VPXORQ:
            return true;
        case XED_ICLASS_VXORPD:
            return true;
        case XED_ICLASS_VXORPS:
            return true;
        case XED_ICLASS_XOR:
            return true;
        case XED_ICLASS_PXOR:
            return true;
        case XED_ICLASS_XORPD:
            return true;
        case XED_ICLASS_XORPS:
            return true;
        case XED_ICLASS_XOR_LOCK:
            return true;
        case XED_ICLASS_NOT:
            return true;
        case XED_ICLASS_NOT_LOCK:
            return true;
        case XED_ICLASS_OR:
            return true;
        case XED_ICLASS_ORPD:
            return true;
        case XED_ICLASS_ORPS:
            return true;
        case XED_ICLASS_OR_LOCK:
            return true;
        case XED_ICLASS_CVTDQ2PD:
            return true;
        case XED_ICLASS_CVTDQ2PS:
            return true;
        case XED_ICLASS_CVTPD2DQ:
            return true;
        case XED_ICLASS_CVTPD2PI:
            return true;
        case XED_ICLASS_CVTPD2PS:
            return true;
        case XED_ICLASS_CVTPI2PD:
            return true;
        case XED_ICLASS_CVTPI2PS:
            return true;
        case XED_ICLASS_CVTPS2DQ:
            return true;
        case XED_ICLASS_CVTPS2PD:
            return true;
        case XED_ICLASS_CVTPS2PI:
            return true;
        case XED_ICLASS_CVTSD2SI:
            return true;
        case XED_ICLASS_CVTSD2SS:
            return true;
        case XED_ICLASS_CVTSI2SD:
            return true;
        case XED_ICLASS_CVTSI2SS:
            return true;
        case XED_ICLASS_CVTSS2SD:
            return true;
        case XED_ICLASS_CVTSS2SI:
            return true;
        case XED_ICLASS_CVTTPD2DQ:
            return true;
        case XED_ICLASS_CVTTPD2PI:
            return true;
        case XED_ICLASS_CVTTPS2DQ:
            return true;
        case XED_ICLASS_CVTTPS2PI:
            return true;
        case XED_ICLASS_CVTTSD2SI:
            return true;
        case XED_ICLASS_CVTTSS2SI:
            return true;
        case XED_ICLASS_VCVTDQ2PD:
            return true;
        case XED_ICLASS_VCVTDQ2PH:
            return true;
        case XED_ICLASS_VCVTDQ2PS:
            return true;
        case XED_ICLASS_VCVTNE2PS2BF16:
            return true;
        case XED_ICLASS_VCVTNEPS2BF16:
            return true;
        case XED_ICLASS_VCVTPD2DQ:
            return true;
        case XED_ICLASS_VCVTPD2PH:
            return true;
        case XED_ICLASS_VCVTPD2PS:
            return true;
        case XED_ICLASS_VCVTPD2QQ:
            return true;
        case XED_ICLASS_VCVTPD2UDQ:
            return true;
        case XED_ICLASS_VCVTPD2UQQ:
            return true;
        case XED_ICLASS_VCVTPH2DQ:
            return true;
        case XED_ICLASS_VCVTPH2PD:
            return true;
        case XED_ICLASS_VCVTPH2PS:
            return true;
        case XED_ICLASS_VCVTPH2PSX:
            return true;
        case XED_ICLASS_VCVTPH2QQ:
            return true;
        case XED_ICLASS_VCVTPH2UDQ:
            return true;
        case XED_ICLASS_VCVTPH2UQQ:
            return true;
        case XED_ICLASS_VCVTPH2UW:
            return true;
        case XED_ICLASS_VCVTPH2W:
            return true;
        case XED_ICLASS_VCVTPS2DQ:
            return true;
        case XED_ICLASS_VCVTPS2PD:
            return true;
        case XED_ICLASS_VCVTPS2PH:
            return true;
        case XED_ICLASS_VCVTPS2PHX:
            return true;
        case XED_ICLASS_VCVTPS2QQ:
            return true;
        case XED_ICLASS_VCVTPS2UDQ:
            return true;
        case XED_ICLASS_VCVTPS2UQQ:
            return true;
        case XED_ICLASS_VCVTQQ2PD:
            return true;
        case XED_ICLASS_VCVTQQ2PH:
            return true;
        case XED_ICLASS_VCVTQQ2PS:
            return true;
        case XED_ICLASS_VCVTSD2SH:
            return true;
        case XED_ICLASS_VCVTSD2SI:
            return true;
        case XED_ICLASS_VCVTSD2SS:
            return true;
        case XED_ICLASS_VCVTSD2USI:
            return true;
        case XED_ICLASS_VCVTSH2SD:
            return true;
        case XED_ICLASS_VCVTSH2SI:
            return true;
        case XED_ICLASS_VCVTSH2SS:
            return true;
        case XED_ICLASS_VCVTSH2USI:
            return true;
        case XED_ICLASS_VCVTSI2SD:
            return true;
        case XED_ICLASS_VCVTSI2SH:
            return true;
        case XED_ICLASS_VCVTSI2SS:
            return true;
        case XED_ICLASS_VCVTSS2SD:
            return true;
        case XED_ICLASS_VCVTSS2SH:
            return true;
        case XED_ICLASS_VCVTSS2SI:
            return true;
        case XED_ICLASS_VCVTSS2USI:
            return true;
        case XED_ICLASS_VCVTTPD2DQ:
            return true;
        case XED_ICLASS_VCVTTPD2QQ:
            return true;
        case XED_ICLASS_VCVTTPD2UDQ:
            return true;
        case XED_ICLASS_VCVTTPD2UQQ:
            return true;
        case XED_ICLASS_VCVTTPH2DQ:
            return true;
        case XED_ICLASS_VCVTTPH2QQ:
            return true;
        case XED_ICLASS_VCVTTPH2UDQ:
            return true;
        case XED_ICLASS_VCVTTPH2UQQ:
            return true;
        case XED_ICLASS_VCVTTPH2UW:
            return true;
        case XED_ICLASS_VCVTTPH2W:
            return true;
        case XED_ICLASS_VCVTTPS2DQ:
            return true;
        case XED_ICLASS_VCVTTPS2QQ:
            return true;
        case XED_ICLASS_VCVTTPS2UDQ:
            return true;
        case XED_ICLASS_VCVTTPS2UQQ:
            return true;
        case XED_ICLASS_VCVTTSD2SI:
            return true;
        case XED_ICLASS_VCVTTSD2USI:
            return true;
        case XED_ICLASS_VCVTTSH2SI:
            return true;
        case XED_ICLASS_VCVTTSH2USI:
            return true;
        case XED_ICLASS_VCVTTSS2SI:
            return true;
        case XED_ICLASS_VCVTTSS2USI:
            return true;
        case XED_ICLASS_VCVTUDQ2PD:
            return true;
        case XED_ICLASS_VCVTUDQ2PH:
            return true;
        case XED_ICLASS_VCVTUDQ2PS:
            return true;
        case XED_ICLASS_VCVTUQQ2PD:
            return true;
        case XED_ICLASS_VCVTUQQ2PH:
            return true;
        case XED_ICLASS_VCVTUQQ2PS:
            return true;
        case XED_ICLASS_VCVTUSI2SD:
            return true;
        case XED_ICLASS_VCVTUSI2SH:
            return true;
        case XED_ICLASS_VCVTUSI2SS:
            return true;
        case XED_ICLASS_VCVTUW2PH:
            return true;
        case XED_ICLASS_VCVTW2PH:
            return true;
        case XED_ICLASS_CMP:
            return true; 	
        case XED_ICLASS_CMPPD:
            return true; 	
        case XED_ICLASS_CMPPS:
            return true; 	
        case XED_ICLASS_CMPSB:
            return true; 	
        case XED_ICLASS_CMPSD:
            return true; 	
        case XED_ICLASS_CMPSD_XMM:
            return true; 	
        case XED_ICLASS_CMPSQ:
            return true; 	
        case XED_ICLASS_CMPSS:
            return true; 	
        case XED_ICLASS_CMPSW:
            return true; 	
        case XED_ICLASS_CMPXCHG:
            return true; 	
        case XED_ICLASS_CMPXCHG16B:
            return true; 	
        case XED_ICLASS_CMPXCHG16B_LOCK:
            return true; 	
        case XED_ICLASS_CMPXCHG8B:
            return true; 	
        case XED_ICLASS_CMPXCHG8B_LOCK:
            return true; 	
        case XED_ICLASS_CMPXCHG_LOCK:
            return true; 	
        case XED_ICLASS_PCMPEQB:
            return true; 	
        case XED_ICLASS_PCMPEQD:
            return true; 	
        case XED_ICLASS_PCMPEQQ:
            return true; 	
        case XED_ICLASS_PCMPEQW:
            return true; 	
        case XED_ICLASS_PCMPESTRI:
            return true; 	
        case XED_ICLASS_PCMPESTRI64:
            return true; 	
        case XED_ICLASS_PCMPESTRM:
            return true; 	
        case XED_ICLASS_PCMPESTRM64:
            return true; 	
        case XED_ICLASS_PCMPGTB:
            return true; 	
        case XED_ICLASS_PCMPGTD:
            return true; 	
        case XED_ICLASS_PCMPGTQ:
            return true; 	
        case XED_ICLASS_PCMPGTW:
            return true; 	
        case XED_ICLASS_PCMPISTRI:
            return true; 	
        case XED_ICLASS_PCMPISTRI64:
            return true; 	
        case XED_ICLASS_PCMPISTRM:
            return true; 
        case XED_ICLASS_PFCMPEQ:
            return true; 	
        case XED_ICLASS_PFCMPGE:
            return true; 	
        case XED_ICLASS_PFCMPGT:
            return true; 	
        case XED_ICLASS_REPE_CMPSB:
            return true; 	
        case XED_ICLASS_REPE_CMPSD:
            return true; 	
        case XED_ICLASS_REPE_CMPSQ:
            return true; 	
        case XED_ICLASS_REPE_CMPSW:
            return true; 	
        case XED_ICLASS_REPNE_CMPSB:
            return true; 	
        case XED_ICLASS_REPNE_CMPSD:
            return true; 	
        case XED_ICLASS_REPNE_CMPSQ:
            return true; 	
        case XED_ICLASS_REPNE_CMPSW:
            return true; 	
        case XED_ICLASS_VCMPPD:
            return true; 	
        case XED_ICLASS_VCMPPH:
            return true; 	
        case XED_ICLASS_VCMPPS:
            return true; 	
        case XED_ICLASS_VCMPSD:
            return true; 	
        case XED_ICLASS_VCMPSH:
            return true; 	
        case XED_ICLASS_VCMPSS:
            return true; 
        case XED_ICLASS_VPCMPB:
            return true; 	
        case XED_ICLASS_VPCMPD:
            return true; 	
        case XED_ICLASS_VPCMPEQB:
            return true; 	
        case XED_ICLASS_VPCMPEQD:
            return true; 	
        case XED_ICLASS_VPCMPEQQ:
            return true; 	
        case XED_ICLASS_VPCMPEQW:
            return true; 	
        case XED_ICLASS_VPCMPESTRI:
            return true; 	
        case XED_ICLASS_VPCMPESTRI64:
            return true; 	
        case XED_ICLASS_VPCMPESTRM:
            return true; 	
        case XED_ICLASS_VPCMPESTRM64:
            return true; 	
        case XED_ICLASS_VPCMPGTB:
            return true; 	
        case XED_ICLASS_VPCMPGTD:
            return true; 	
        case XED_ICLASS_VPCMPGTQ:
            return true; 	
        case XED_ICLASS_VPCMPGTW:
            return true; 	
        case XED_ICLASS_VPCMPISTRI:
            return true; 	
        case XED_ICLASS_VPCMPISTRI64:
            return true; 	
        case XED_ICLASS_VPCMPISTRM:
            return true; 	
        case XED_ICLASS_VPCMPQ:
            return true; 	
        case XED_ICLASS_VPCMPUB:
            return true; 	
        case XED_ICLASS_VPCMPUD:
            return true; 	
        case XED_ICLASS_VPCMPUQ:
            return true; 	
        case XED_ICLASS_VPCMPUW:
            return true; 	
        case XED_ICLASS_VPCMPW:
            return true;
    }
    return false;
}

bool ignore_instruction( OPCODE iclass ){

    switch (iclass){
        case XED_ICLASS_CALL_NEAR:
            return true;
        case XED_ICLASS_CALL_FAR:
            return true;
        case XED_ICLASS_FDISI8087_NOP:
            return true;
        case XED_ICLASS_FENI8087_NOP:
            return true;
        case XED_ICLASS_FNOP:
            return true;
        case XED_ICLASS_FSETPM287_NOP:
            return true;
        case XED_ICLASS_NOP:
            return true;
        case XED_ICLASS_NOP2:
            return true;
        case XED_ICLASS_NOP3:
            return true;
        case XED_ICLASS_NOP4:
            return true;
        case XED_ICLASS_NOP5:
            return true;
        case XED_ICLASS_NOP6:
            return true;
        case XED_ICLASS_NOP7:
            return true;
        case XED_ICLASS_NOP8:
            return true;
        case XED_ICLASS_NOP9:
            return true;
    }
    return false;
}

bool is_minmax( OPCODE iclass ){

    switch (iclass){
        case XED_ICLASS_MAXSS:
            return true;
        case XED_ICLASS_MAXPS:
            return true;
        case XED_ICLASS_MINSS:
            return true;
        case XED_ICLASS_MINPS:
            return true;
        case XED_ICLASS_MAXSD:
            return true;
        case XED_ICLASS_MAXPD:
            return true;
        case XED_ICLASS_MINSD:
            return true;
        case XED_ICLASS_MINPD:
            return true;
    }
    return false;
}

#define SSE  1
#define SSE2 1
#define SSE3 1
#define SSE4 1
#define AVX  1
#define FMA3 1
#define FMA4 1
// #define AVX512 // this appears to be a flag provided at compile-time

InjectableType is_EV_generator_reg( OPCODE iclass ){

    switch (iclass){
#if SSE > 0
        case XED_ICLASS_ADDSS:
            return ADD;
        case XED_ICLASS_ADDPS:
            return ADD;
        case XED_ICLASS_SUBSS:
            return SUB;
        case XED_ICLASS_SUBPS:
            return SUB;
        case XED_ICLASS_MULSS:
            return MUL;
        case XED_ICLASS_MULPS:
            return MUL;
        case XED_ICLASS_DIVSS:
            return DIV;
        case XED_ICLASS_DIVPS:
            return DIV;
        case XED_ICLASS_SQRTSS:
            return SQRT;
        case XED_ICLASS_SQRTPS:
            return SQRT;
#endif
#if SSE2 > 0
        case XED_ICLASS_ADDSD:
            return ADD;
        case XED_ICLASS_ADDPD:
            return ADD;
        case XED_ICLASS_SUBSD:
            return SUB;
        case XED_ICLASS_SUBPD:
            return SUB;
        case XED_ICLASS_MULSD:
            return MUL;
        case XED_ICLASS_MULPD:
            return MUL;
        case XED_ICLASS_DIVSD:
            return DIV;
        case XED_ICLASS_DIVPD:
            return DIV;
        case XED_ICLASS_SQRTSD:
            return SQRT;
        case XED_ICLASS_SQRTPD:
            return SQRT;
#endif
#if SSE3 > 0
        case XED_ICLASS_ADDSUBPS:
            return ADDSUB;
        case XED_ICLASS_ADDSUBPD:
            return ADDSUB;
        case XED_ICLASS_HADDPS:
            return ADD;
        case XED_ICLASS_HADDPD:
            return ADD;
        case XED_ICLASS_HSUBPS:
            return SUB;
        case XED_ICLASS_HSUBPD:
            return SUB;
#endif
#if SSE4 > 0
        case XED_ICLASS_DPPS:
            return MUL;
        case XED_ICLASS_DPPD:
            return MUL;
#endif
#if AVX > 0
        case XED_ICLASS_VADDSS:
            return ADD;
        case XED_ICLASS_VADDPS:
            return ADD;
        case XED_ICLASS_VADDSD:
            return ADD;
        case XED_ICLASS_VADDPD:
            return ADD;
        case XED_ICLASS_VSUBSS:
            return SUB;
        case XED_ICLASS_VSUBPS:
            return SUB;
        case XED_ICLASS_VSUBSD:
            return SUB;
        case XED_ICLASS_VSUBPD:
            return SUB;
        case XED_ICLASS_VMULSS:
            return MUL;
        case XED_ICLASS_VMULPS:
            return MUL;
        case XED_ICLASS_VMULSD:
            return MUL;
        case XED_ICLASS_VMULPD:
            return MUL;
        case XED_ICLASS_VDIVSS:
            return DIV;
        case XED_ICLASS_VDIVPS:
            return DIV;
        case XED_ICLASS_VDIVSD:
            return DIV;
        case XED_ICLASS_VDIVPD:
            return DIV;
        case XED_ICLASS_VSQRTPS:
            return SQRT;
        case XED_ICLASS_VSQRTPD:
            return SQRT;
        case XED_ICLASS_VSQRTSS:
            return SQRT;
        case XED_ICLASS_VSQRTSD:
            return SQRT;
        case XED_ICLASS_VADDSUBPS:
            return ADDSUB;
        case XED_ICLASS_VADDSUBPD:
            return ADDSUB;
        case XED_ICLASS_VHADDPS:
            return ADD;
        case XED_ICLASS_VHADDPD:
            return ADD;
        case XED_ICLASS_VHSUBPS:
            return SUB;
        case XED_ICLASS_VHSUBPD:
            return SUB;
        case XED_ICLASS_VDPPS:
            return MUL;
        case XED_ICLASS_VDPPD:
            return MUL;
#endif
#if FMA3 > 0
        case XED_ICLASS_VFMADD132PS:
            return FMA;
        case XED_ICLASS_VFMADD213PS:
            return FMA;
        case XED_ICLASS_VFMADD231PS:
            return FMA;
        case XED_ICLASS_VFMADD132SS:
            return FMA;
        case XED_ICLASS_VFMADD213SS:
            return FMA;
        case XED_ICLASS_VFMADD231SS:
            return FMA;
        case XED_ICLASS_VFMADDSUB132PS:
            return FMA;
        case XED_ICLASS_VFMADDSUB213PS:
            return FMA;
        case XED_ICLASS_VFMADDSUB231PS:
            return FMA;
        case XED_ICLASS_VFMSUB132PS:
            return FMA;
        case XED_ICLASS_VFMSUB132SS:
            return FMA;
        case XED_ICLASS_VFMSUB213PS:
            return FMA;
        case XED_ICLASS_VFMSUB213SS:
            return FMA;
        case XED_ICLASS_VFMSUB231PS:
            return FMA;
        case XED_ICLASS_VFMSUB231SS:
            return FMA;
        case XED_ICLASS_VFMSUBADD132PS:
            return FMA;
        case XED_ICLASS_VFMSUBADD213PS:
            return FMA;
        case XED_ICLASS_VFMSUBADD231PS:
            return FMA;
        case XED_ICLASS_VFNMADD132PS:
            return FMA;
        case XED_ICLASS_VFNMADD132SS:
            return FMA;
        case XED_ICLASS_VFNMADD213PS:
            return FMA;
        case XED_ICLASS_VFNMADD213SS:
            return FMA;
        case XED_ICLASS_VFNMADD231PS:
            return FMA;
        case XED_ICLASS_VFNMADD231SS:
            return FMA;
        case XED_ICLASS_VFNMSUB132PS:
            return FMA;
        case XED_ICLASS_VFNMSUB132SS:
            return FMA;
        case XED_ICLASS_VFNMSUB213PS:
            return FMA;
        case XED_ICLASS_VFNMSUB213SS:
            return FMA;
        case XED_ICLASS_VFNMSUB231PS:
            return FMA;
        case XED_ICLASS_VFNMSUB231SS:
            return FMA;
        case XED_ICLASS_VFMADD132PD:
            return FMA;
        case XED_ICLASS_VFMADD213PD:
            return FMA;
        case XED_ICLASS_VFMADD231PD:
            return FMA;
        case XED_ICLASS_VFMADD132SD:
            return FMA;
        case XED_ICLASS_VFMADD213SD:
            return FMA;
        case XED_ICLASS_VFMADD231SD:
            return FMA;
        case XED_ICLASS_VFMADDSUB132PD:
            return FMA;
        case XED_ICLASS_VFMADDSUB213PD:
            return FMA;
        case XED_ICLASS_VFMADDSUB231PD:
            return FMA;
        case XED_ICLASS_VFMSUB132PD:
            return FMA;
        case XED_ICLASS_VFMSUB132SD:
            return FMA;
        case XED_ICLASS_VFMSUB213PD:
            return FMA;
        case XED_ICLASS_VFMSUB213SD:
            return FMA;
        case XED_ICLASS_VFMSUB231PD:
            return FMA;
        case XED_ICLASS_VFMSUB231SD:
            return FMA;
        case XED_ICLASS_VFMSUBADD132PD:
            return FMA;
        case XED_ICLASS_VFMSUBADD213PD:
            return FMA;
        case XED_ICLASS_VFMSUBADD231PD:
            return FMA;
        case XED_ICLASS_VFNMADD132PD:
            return FMA;
        case XED_ICLASS_VFNMADD132SD:
            return FMA;
        case XED_ICLASS_VFNMADD213PD:
            return FMA;
        case XED_ICLASS_VFNMADD213SD:
            return FMA;
        case XED_ICLASS_VFNMADD231PD:
            return FMA;
        case XED_ICLASS_VFNMADD231SD:
            return FMA;
        case XED_ICLASS_VFNMSUB132PD:
            return FMA;
        case XED_ICLASS_VFNMSUB132SD:
            return FMA;
        case XED_ICLASS_VFNMSUB213PD:
            return FMA;
        case XED_ICLASS_VFNMSUB213SD:
            return FMA;
        case XED_ICLASS_VFNMSUB231PD:
            return FMA;
        case XED_ICLASS_VFNMSUB231SD:
            return FMA;
#endif
#if FMA4 > 0
        case XED_ICLASS_VFMADDSS:
            return FMA;
        case XED_ICLASS_VFMADDPS:
            return FMA;
        case XED_ICLASS_VFMSUBADDPS:
            return FMA;
        case XED_ICLASS_VFMSUBPS:
            return FMA;
        case XED_ICLASS_VFMSUBSS:
            return FMA;
        case XED_ICLASS_VFNMADDPS:
            return FMA;
        case XED_ICLASS_VFNMADDSS:
            return FMA;
        case XED_ICLASS_VFNMSUBPS:
            return FMA;
        case XED_ICLASS_VFNMSUBSS:
            return FMA;
        case XED_ICLASS_VFMADDSD:
            return FMA;
        case XED_ICLASS_VFMADDPD:
            return FMA;
        case XED_ICLASS_VFMSUBADDPD:
            return FMA;
        case XED_ICLASS_VFMSUBPD:
            return FMA;
        case XED_ICLASS_VFMSUBSD:
            return FMA;
        case XED_ICLASS_VFNMADDPD:
            return FMA;
        case XED_ICLASS_VFNMADDSD:
            return FMA;
        case XED_ICLASS_VFNMSUBPD:
            return FMA;
        case XED_ICLASS_VFNMSUBSD:
            return FMA;
#endif
#ifdef AVX512
        case XED_ICLASS_V4FMADDPS:
            return FMA;
        case XED_ICLASS_V4FMADDSS:
            return FMA;
        case XED_ICLASS_V4FNMADDPS:
            return FMA;
        case XED_ICLASS_V4FNMADDSS:
            return FMA;
        case XED_ICLASS_V4FMADDPD:
            return FMA;
        case XED_ICLASS_V4FMADDSD:
            return FMA;
        case XED_ICLASS_V4FNMADDPD:
            return FMA;
        case XED_ICLASS_V4FNMADDSD:
            return FMA;
#endif
    }
    return NOT_INJECTABLE;
}

std::string opcode_to_smtlib2( OPCODE iclass ){

    switch (iclass){
        case XED_ICLASS_RCPPS:
            return "$rcp";
        case XED_ICLASS_RCPSS:
            return "$rcp";
        case XED_ICLASS_VRCPPS:
            return "$rcp";
        case XED_ICLASS_VRCPSS:
            return "$rcp";

        case XED_ICLASS_ADDSS:
            return "fp.add";
        case XED_ICLASS_ADDPS:
            return "fp.add";
        case XED_ICLASS_ADDSD:
            return "fp.add";
        case XED_ICLASS_ADDPD:
            return "fp.add";
        case XED_ICLASS_VADDSS:
            return "fp.add";
        case XED_ICLASS_VADDPS:
            return "fp.add";
        case XED_ICLASS_VADDSD:
            return "fp.add";
        case XED_ICLASS_VADDPD:
            return "fp.add";

        case XED_ICLASS_HADDPS:
            return "$hadd";
        case XED_ICLASS_HADDPD:
            return "$hadd";
        case XED_ICLASS_VHADDPS:
            return "$hadd";
        case XED_ICLASS_VHADDPD:
            return "$hadd";

        case XED_ICLASS_SUBSS:
            return "fp.sub";
        case XED_ICLASS_SUBPS:
            return "fp.sub";
        case XED_ICLASS_SUBSD:
            return "fp.sub";
        case XED_ICLASS_SUBPD:
            return "fp.sub";
        case XED_ICLASS_VSUBSS:
            return "fp.sub";
        case XED_ICLASS_VSUBPS:
            return "fp.sub";
        case XED_ICLASS_VSUBSD:
            return "fp.sub";
        case XED_ICLASS_VSUBPD:
            return "fp.sub";

        case XED_ICLASS_MULSS:
            return "fp.mul";
        case XED_ICLASS_MULPS:
            return "fp.mul";
        case XED_ICLASS_MULSD:
            return "fp.mul";
        case XED_ICLASS_MULPD:
            return "fp.mul";
        case XED_ICLASS_VMULSS:
            return "fp.mul";
        case XED_ICLASS_VMULPS:
            return "fp.mul";
        case XED_ICLASS_VMULSD:
            return "fp.mul";
        case XED_ICLASS_VMULPD:
            return "fp.mul";

        case XED_ICLASS_DIVSS:
            return "fp.div";
        case XED_ICLASS_DIVPS:
            return "fp.div";
        case XED_ICLASS_DIVSD:
            return "fp.div";
        case XED_ICLASS_DIVPD:
            return "fp.div";
        case XED_ICLASS_VDIVSS:
            return "fp.div";
        case XED_ICLASS_VDIVPS:
            return "fp.div";
        case XED_ICLASS_VDIVSD:
            return "fp.div";
        case XED_ICLASS_VDIVPD:
            return "fp.div";

        case XED_ICLASS_SQRTSS:
            return "fp.sqrt";
        case XED_ICLASS_SQRTPS:
            return "fp.sqrt";
        case XED_ICLASS_SQRTSD:
            return "fp.sqrt";
        case XED_ICLASS_SQRTPD:
            return "fp.sqrt";
        case XED_ICLASS_VSQRTSS:
            return "fp.sqrt";
        case XED_ICLASS_VSQRTSD:
            return "fp.sqrt";
        case XED_ICLASS_VSQRTPS:
            return "fp.sqrt";
        case XED_ICLASS_VSQRTPD:
            return "fp.sqrt";

        case XED_ICLASS_RSQRTSS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_RSQRTPS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT14PD:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT14PS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT14SD:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT14SS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT28PD:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT28PS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT28SD:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRT28SS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRTPH:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRTPS:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRTSH:
            return "ERROR: reciprocal sqrt not yet supported";
        case XED_ICLASS_VRSQRTSS:
            return "ERROR: reciprocal sqrt not yet supported";

        case XED_ICLASS_MAXSS:
            return "fp.max";
        case XED_ICLASS_MAXPS:
            return "fp.max";
        case XED_ICLASS_MAXSD:
            return "fp.max";
        case XED_ICLASS_MAXPD:
            return "fp.max";
        case XED_ICLASS_VMAXSS:
            return "fp.max";
        case XED_ICLASS_VMAXPS:
            return "fp.max";
        case XED_ICLASS_VMAXSD:
            return "fp.max";
        case XED_ICLASS_VMAXPD:
            return "fp.max";

        case XED_ICLASS_MINSS:
            return "fp.min";
        case XED_ICLASS_MINPS:
            return "fp.min";
        case XED_ICLASS_MINSD:
            return "fp.min";
        case XED_ICLASS_MINPD:
            return "fp.min";
        case XED_ICLASS_VMINSS:
            return "fp.min";
        case XED_ICLASS_VMINPS:
            return "fp.min";
        case XED_ICLASS_VMINSD:
            return "fp.min";
        case XED_ICLASS_VMINPD:
            return "fp.min";

        // comparisons parameterized by an immediate that write to a vector registor
        case XED_ICLASS_CMPPD:
            return "$cmp"; 	
        case XED_ICLASS_CMPPS:
            return "$cmp"; 	
        case XED_ICLASS_CMPSS:
            return "$cmp"; 	
        case XED_ICLASS_CMPSD:
            return "$cmp";
        case XED_ICLASS_VCMPPD:
            return "$cmp"; 	
        case XED_ICLASS_VCMPPS:
            return "$cmp"; 	
        case XED_ICLASS_VCMPSD:
            return "$cmp";
        case XED_ICLASS_VCMPSS:
            return "$cmp"; 

        // comparisons that set EFLAGS
        case XED_ICLASS_COMISD:
            return "$cmp";
        case XED_ICLASS_COMISS:
            return "$cmp";
        case XED_ICLASS_UCOMISD:
            return "$cmp";
        case XED_ICLASS_UCOMISS:
            return "$cmp";
        case XED_ICLASS_VCOMISD:
            return "$cmp";
        case XED_ICLASS_VCOMISS:
            return "$cmp";
        case XED_ICLASS_VUCOMISD:
            return "$cmp";
        case XED_ICLASS_VUCOMISS:
            return "$cmp";

        // byte comparisons using vector registers
        case XED_ICLASS_PCMPEQB:
            return "$cmp"; 	
        case XED_ICLASS_PCMPEQD:
            return "$cmp"; 	
        case XED_ICLASS_PCMPEQQ:
            return "$cmp"; 	
        case XED_ICLASS_PCMPEQW:
            return "$cmp"; 	
        case XED_ICLASS_PCMPGTB:
            return "$cmp"; 	
        case XED_ICLASS_PCMPGTD:
            return "$cmp"; 	
        case XED_ICLASS_PCMPGTQ:
            return "$cmp"; 	
        case XED_ICLASS_PCMPGTW:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPEQB:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPEQD:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPEQQ:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPEQW:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPGTB:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPGTD:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPGTQ:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPGTW:
            return "$cmp"; 	

        // deprecated FP extension: https://en.wikipedia.org/wiki/3DNow!
        case XED_ICLASS_PFCMPEQ:
            return "ERROR: deprecated 3DNow FP extension not supported"; 	
        case XED_ICLASS_PFCMPGE:
            return "ERROR: deprecated 3DNow FP extension not supported"; 	
        case XED_ICLASS_PFCMPGT:
            return "ERROR: deprecated 3DNow FP extension not supported"; 	

        // 16 bit operands
        case XED_ICLASS_VCMPSH:
            return "ERROR: 16-bit operands not yet supported"; 	
        case XED_ICLASS_VCMPPH:
            return "ERROR: 16-bit operands not yet supported"; 	
        case XED_ICLASS_VCOMISH:
            return "ERROR: 16-bit operands not yet supported"; 	
        case XED_ICLASS_VUCOMISH:
            return "ERROR: 16-bit operands not yet supported"; 	

#ifdef AVX512
        case XED_ICLASS_VPCMPB:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPUB:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPD:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPUD:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPQ:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPUQ:
            return "$cmp"; 	
        case XED_ICLASS_VPCMPW:
            return "$cmp";
        case XED_ICLASS_VPCMPUW:
            return "$cmp";
#endif

        case XED_ICLASS_ANDPD:
            return "$bvand";
        case XED_ICLASS_ANDPS:
            return "$bvand";
        case XED_ICLASS_VANDPD:
            return "$bvand";
        case XED_ICLASS_VANDPS:
            return "$bvand";

        case XED_ICLASS_ANDNPD:
            return "$bvandn";
        case XED_ICLASS_ANDNPS:
            return "$bvandn";
        case XED_ICLASS_VANDNPD:
            return "$bvandn";
        case XED_ICLASS_VANDNPS:
            return "$bvandn";

        case XED_ICLASS_PAND:
            return "$bvand";
        case XED_ICLASS_PANDN:
            return "$bvandn";

        case XED_ICLASS_ORPD:
            return "$bvor";
        case XED_ICLASS_ORPS:
            return "$bvor";
        case XED_ICLASS_VORPD:
            return "$bvor";
        case XED_ICLASS_VORPS:
            return "$bvor";

        case XED_ICLASS_XORPD:
            return "$bvxor";
        case XED_ICLASS_XORPS:
            return "$bvxor";
        case XED_ICLASS_VXORPS:
            return "$bvxor";
        case XED_ICLASS_VXORPD:
            return "$bvxor";

        case XED_ICLASS_PXOR:
            return "$bvxor";
        case XED_ICLASS_VPXOR:
            return "$bvxor";

#ifdef AVX512
        case XED_ICLASS_VPXORD:
            return "$bvxor";
        case XED_ICLASS_VPXORQ:
            return "$bvxor";
#endif

        case XED_ICLASS_CVTPS2PD:
            return "$cvt";
        case XED_ICLASS_CVTPD2PS:
            return "$cvt";
        case XED_ICLASS_CVTSD2SS:
            return "$cvt";
        case XED_ICLASS_CVTSS2SD:
            return "$cvt";
        case XED_ICLASS_VCVTPD2PS:
            return "$cvt";
        case XED_ICLASS_VCVTPS2PD:
            return "$cvt";
        case XED_ICLASS_VCVTSD2SS:
            return "$cvt";
        case XED_ICLASS_VCVTSS2SD:
            return "$cvt";

        case XED_ICLASS_BLENDPD:
            return "$blend";
        case XED_ICLASS_BLENDPS:
            return "$blend";
        case XED_ICLASS_VBLENDPD:
            return "$blend";
        case XED_ICLASS_VBLENDPS:
            return "$blend";

        case XED_ICLASS_BLENDVPD:
            return "ERROR: variable blend not yet supported";
        case XED_ICLASS_BLENDVPS:
            return "ERROR: variable blend not yet supported";
        case XED_ICLASS_VBLENDVPD:
            return "ERROR: variable blend not yet supported";
        case XED_ICLASS_VBLENDVPS:
            return "ERROR: variable blend not yet supported";

        case XED_ICLASS_PSHUFD:
            return "$pshuf";
        case XED_ICLASS_VPSHUFD:
            return "$pshuf";

        case XED_ICLASS_PSHUFHW:
            return "ERROR: shufhw not yet supported";
        case XED_ICLASS_PSHUFLW:
            return "ERROR: shuflw not yet supported";
        case XED_ICLASS_VPSHUFHW:
            return "ERROR: shufhw not yet supported";
        case XED_ICLASS_VPSHUFLW:
            return "ERROR: shuflw not yet supported";

        case XED_ICLASS_SHUFPD:
            return "$shufp";
        case XED_ICLASS_SHUFPS:
            return "$shufp";
        case XED_ICLASS_VSHUFPD:
            return "$shufp";
        case XED_ICLASS_VSHUFPS:
            return "$shufp";

        case XED_ICLASS_VPERMILPS:
            return "$permil";
        case XED_ICLASS_VPERMILPD:
            return "$permil";
        case XED_ICLASS_VPERM2F128:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMI2PD:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMI2PS:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMPD:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMPS:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMT2PD:
            return "ERROR: perm not supported";
        case XED_ICLASS_VPERMT2PS:
            return "ERROR: perm not supported";

        case XED_ICLASS_UNPCKHPD:
            return "$unpckhp";
        case XED_ICLASS_UNPCKHPS:
            return "$unpckhp";
        case XED_ICLASS_VUNPCKHPD:
            return "$unpckhp";
        case XED_ICLASS_VUNPCKHPS:
            return "$unpckhp";

        case XED_ICLASS_UNPCKLPD:
            return "$unpcklp";
        case XED_ICLASS_UNPCKLPS:
            return "$unpcklp";
        case XED_ICLASS_VUNPCKLPD:
            return "$unpcklp";
        case XED_ICLASS_VUNPCKLPS:
            return "$unpcklp";

        case XED_ICLASS_MOVSS:
            return "$mov";
        case XED_ICLASS_MOVSD:
            return "$mov";
        case XED_ICLASS_MOVSD_XMM:
            return "$mov";
        case XED_ICLASS_MOVAPD:
            return "$mov";
        case XED_ICLASS_MOVAPS:
            return "$mov";
        case XED_ICLASS_MOVUPD:
            return "$mov";
        case XED_ICLASS_MOVUPS:
            return "$mov";
        case XED_ICLASS_VMOVSS:
            return "$mov";
        case XED_ICLASS_VMOVSD:
            return "$mov";
        case XED_ICLASS_VMOVAPD:
            return "$mov";
        case XED_ICLASS_VMOVAPS:
            return "$mov";
        case XED_ICLASS_VMOVUPD:
            return "$mov";
        case XED_ICLASS_VMOVUPS:
            return "$mov";

        case XED_ICLASS_VMOVDQA:
            return "$mov";
        case XED_ICLASS_VMOVDQU:
            return "$mov";
        case XED_ICLASS_MOVDQA:
            return "$mov";
        case XED_ICLASS_MOVDQU:
            return "$mov";

        case XED_ICLASS_MOVHLPS:
            return "$movhlps";
        case XED_ICLASS_VMOVHLPS:
            return "$movhlps";

        case XED_ICLASS_MOVLHPS:
            return "$movlhps";
        case XED_ICLASS_VMOVLHPS:
            return "$movlhps";

        case XED_ICLASS_MOVHPD:
            return "$movhp";
        case XED_ICLASS_MOVHPS:
            return "$movhp";
        case XED_ICLASS_VMOVHPD:
            return "$movhp";
        case XED_ICLASS_VMOVHPS:
            return "$movhp";

        case XED_ICLASS_MOVLPD:
            return "$movlp";
        case XED_ICLASS_MOVLPS:
            return "$movlp";
        case XED_ICLASS_VMOVLPD:
            return "$movlp";
        case XED_ICLASS_VMOVLPS:
            return "$movlp";

        case XED_ICLASS_MOVSHDUP:
            return "$movshdup";
        case XED_ICLASS_VMOVSHDUP:
            return "$movshdup";

        case XED_ICLASS_MOVSLDUP:
            return "$movevendup";
        case XED_ICLASS_VMOVSLDUP:
            return "$movevendup";
        case XED_ICLASS_MOVDDUP:
            return "$movevendup";
        case XED_ICLASS_VMOVDDUP:
            return "$movevendup";

        case XED_ICLASS_VBROADCASTSD:
            return "$vbroadcasts";
        case XED_ICLASS_VBROADCASTSS:
            return "$vbroadcasts";
        case XED_ICLASS_VBROADCASTF128:
            return "$vbroadcasts";

        case XED_ICLASS_INSERTPS:
            return "$insert";
        case XED_ICLASS_VINSERTPS:
            return "$insert";
        case XED_ICLASS_VINSERTF128:
            return "$vinsertf128";
        case XED_ICLASS_VEXTRACTF128:
            return "$vextractf128";

        case XED_ICLASS_VPSLLQ:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSLLD:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSLLW:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSLLQ:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSLLD:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSLLW:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRLW:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRLD:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRLQ:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSRLW:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSRLD:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_PSRLQ:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRAW:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRAD:
            return "ERROR: shift not yet supported";
        case XED_ICLASS_VPSRAQ:
            return "ERROR: shift not yet supported";

        // conversions involving integer types
        case XED_ICLASS_CVTDQ2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTDQ2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPD2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPD2PI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPI2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPI2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPS2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTPS2PI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTSD2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTSI2SD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTSI2SS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTSS2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTPD2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTPD2PI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTPS2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTPS2PI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTSD2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_CVTTSS2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTDQ2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTDQ2PH:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTDQ2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPD2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPD2QQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPD2UDQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPD2UQQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPH2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPS2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPS2QQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPS2UDQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTPS2UQQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTQQ2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTQQ2PH:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTQQ2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSD2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSD2USI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSI2SD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSI2SS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSS2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTSS2USI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPD2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPD2QQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPD2UDQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPD2UQQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPS2DQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPS2QQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPS2UDQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTPS2UQQ:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTSD2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTSD2USI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTSS2SI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTTSS2USI:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUDQ2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUDQ2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUQQ2PD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUQQ2PS:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUSI2SD:
            return "ERROR: conversion to and from integer types not yet supported";
        case XED_ICLASS_VCVTUSI2SS:
            return "ERROR: conversion to and from integer types not yet supported";

        // 16 bit operands
        case XED_ICLASS_VCVTNE2PS2BF16:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTNEPS2BF16:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPD2PH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2PD:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2PS:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2PSX:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2QQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2UDQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2UQQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2UW:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPH2W:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPS2PH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTPS2PHX:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSD2SH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSH2SD:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSH2SI:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSH2SS:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSH2USI:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSI2SH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTSS2SH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2DQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2QQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2UDQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2UQQ:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2UW:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTPH2W:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTUSI2SH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTUW2PH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTW2PH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTUQQ2PH:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTSH2SI:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTTSH2USI:
            return "ERROR: 16-bit operands not yet supported";
        case XED_ICLASS_VCVTUDQ2PH:
            return "ERROR: 16-bit operands not yet supported";

        case XED_ICLASS_ADDSUBPS:
            return "ERROR: ADDSUB not yet supported";
        case XED_ICLASS_ADDSUBPD:
            return "ERROR: ADDSUB not yet supported";
        case XED_ICLASS_HSUBPS:
            return "ERROR: HSUB not yet supported";
        case XED_ICLASS_HSUBPD:
            return "ERROR: HSUB not yet supported";
        case XED_ICLASS_DPPS:
            return "ERROR: DP not yet supported";
        case XED_ICLASS_DPPD:
            return "ERROR: DP not yet supported";
        case XED_ICLASS_VADDSUBPS:
            return "ERROR: ADDSUB not yet supported";
        case XED_ICLASS_VADDSUBPD:
            return "ERROR: ADDSUB not yet supported";
        case XED_ICLASS_VHSUBPS:
            return "ERROR: HSUB not yet supported";
        case XED_ICLASS_VHSUBPD:
            return "ERROR: HSUB not yet supported";
        case XED_ICLASS_VDPPS:
            return "ERROR: DP not yet supported";
        case XED_ICLASS_VDPPD:
            return "ERROR: DP not yet supported";

        case XED_ICLASS_VFMADD132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD132SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD213SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD231SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB132SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB213SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB231SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD132SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD213SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD231SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB132PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB132SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB213PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB213SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB231PS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB231SS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD132SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD213SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADD231SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSUB231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB132SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB213SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUB231SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADD231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD132SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD213SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADD231SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB132PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB132SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB213PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB213SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB231PD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUB231SD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDPS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADDPS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBPS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBSS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADDPS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADDSS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUBPS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUBSS:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDSD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMADDPD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBADDPD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBPD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFMSUBSD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADDPD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMADDSD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUBPD:
            return "ERROR: FMA not yet supported";
        case XED_ICLASS_VFNMSUBSD:
            return "ERROR: FMA not yet supported";
    }
    return "";
}

//////////////////////////////////////////////////////////////////////////////////
// modified code from  https://rosettacode.org/wiki/Arithmetic_evaluation#C++
//////////////////////////////////////////////////////////////////////////////////
template <class T>
class stack {
    private:
        std::vector<T> st;
        T sentinel;
    public:
        stack() { sentinel = T(); }
        bool empty() { return st.empty(); }
        void push(T info) { st.push_back(info); }
        T& top() {
            if (!st.empty()) {
                return st.back();
            }
            return sentinel;
        }
        T pop() {
            T ret = top();
            if (!st.empty()) st.pop_back();
            return ret;
        }
};

//determine associativity of operator, returns true if left, false if right
bool leftAssociate(char c) {
    switch (c) {
        case '^': return false;
        case '*': return true;
        case '/': return true;
        case '%': return true;
        case '+': return true;
        case '-': return true;
        default:
            break;
    }
    return false;
}

//determins precedence level of operator
int precedence(char c) {
    switch (c) {
        case '^': return 7;
        case '*': return 5;
        case '/': return 5;
        case '%': return 5;
        case '+': return 3;
        case '-': return 3;
        default:
            break;
    }
    return 0; 
}

//converts infix expression std::string to postfix expression std::string
std::string shuntingYard(std::string expr) {
    stack<char> ops;
    std::string output;
    for (char c : expr) {
        if (c == '(') {
            ops.push(c);
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '%') {
                if (precedence(c) < precedence(ops.top()) || 
                   (precedence(c) == precedence(ops.top()) && leftAssociate(c))) {
                    output.push_back(' ');
                    output.push_back(ops.pop());
                    output.push_back(' ');
                    ops.push(c);
                } else {
                    ops.push(c);
                    output.push_back(' ');
                }
        } else if (c == ')') {
            while (!ops.empty()) {
                if (ops.top() != '(') {
                    output.push_back(ops.pop());
                } else {
                    ops.pop();
                    break;
                }
            }
        } else {
            output.push_back(c);
        }
    }
    while (!ops.empty()) 
        if (ops.top() != '(')
            output.push_back(ops.pop());
        else ops.pop();
    return output;
}

struct Token {
    int type;
    union {
        int num;
        char op;
    };
    Token(int n) : type(0), num(n) { }
    Token(char c) : type(1), op(c) { }
};

//converts postfix expression std::string to std::vector of tokens
std::vector<Token> lex(std::string pfExpr) {
    std::vector<Token> tokens;
    for (uint32_t i = 0; i < pfExpr.size(); i++) {
        char c = pfExpr[i];
        if (isdigit(c)) {
            std::string num;
            do {
                num.push_back(c);
                c = pfExpr[++i];
            } while (i < pfExpr.size() && isdigit(c));
            std::istringstream iss(num);
            int temp;
            iss >> temp;
            tokens.push_back(Token(temp));
            i--;
            continue;
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '%') {
            tokens.push_back(Token(c));
        } 
    }
    return tokens;
}

//structure used for nodes of expression tree
struct node {
    Token token;
    node* left;
    node* right;
    node(Token tok) : token(tok), left(nullptr), right(nullptr) { }
};

//builds expression tree from std::vector of tokens
node* buildTree(std::vector<Token> tokens) {
    stack<node*> sf;
    for (uint32_t i = 0; i < tokens.size(); i++) {
        Token c = tokens[i];
        if (c.type == 1) {
            node* x = new node(c);
            x->right = sf.pop();
            x->left = sf.pop();
            sf.push(x);
        } else
        if (c.type == 0) {
            sf.push(new node(c));
            continue;
        }
    }
    return sf.top();
}

//evaluate expression tree, while anotating steps being performed.
int recd = 0;
int eval(node* x) {
    recd++;
    if (x == nullptr) {
        recd--;
        return 0;
    }
    if (x->token.type == 0) {
        recd--;
        return x->token.num;
    }
    if (x->token.type == 1) {
        int lhs = eval(x->left);
        int rhs = eval(x->right);
        recd--;
        switch (x->token.op) {
            case '^': return (int) pow((float)lhs, (float)rhs);
            case '*': return lhs*rhs;
            case '/': 
                if (rhs == 0) {
                    assert(false && "Error: divide by zero");
                } else 
                    return lhs/rhs;
            case '%':
                return (int)lhs % (int)rhs;
            case '+': return lhs+rhs;
            case '-': return lhs-rhs;
            default:
            break;
        }
    }
    return 0;
}

int32_t evaluate_expression_helper( std::string expression ){
    return eval(buildTree(lex(shuntingYard(expression))));
}
//////////////////////////////////////////////////////////////////////////