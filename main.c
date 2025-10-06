#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

#define MAX_IMPORT_FUNCS 64
#define MAX_EXPORT_FUNCS 64

typedef struct {
    uint8_t param_types[16];
    int param_count;
    uint8_t result_types[16];
    int result_count;
} FuncType;

typedef int32_t (*ImportFuncPtr)(int32_t *args, int argc);

typedef struct {
    char *mod_name;
    char *field_name;
    uint32_t type_index;
    int param_count;     // C関数が期待する引数の数
    ImportFuncPtr func;  // 実際に登録されるC関数
} ImportFunc;

typedef struct {
    const char *name;    // export 名 (WASM モジュール上の関数名)
    uint32_t func_idx;   // VM 内関数のインデックス
    uint32_t type_index; // 型インデックス（必要なら）
} ExportFunc;

typedef struct {
    const char *name;
    uint32_t memory_idx;
} MemoryExport;

typedef struct {
    size_t return_pc;    // 呼び出し元に戻るためのPC
    int local_base;      // このフレームのローカル変数の開始インデックス
    int32_t locals[16];  // このフレームのローカル変数のバックアップ
    int sp_base;         // このフレームのスタックポインタのベース
} CallFrame;

typedef struct {
    size_t start_pc; // loop/ifの開始位置
    size_t end_pc;   // endの次の位置
    size_t else_pc;  // elseの次の位置 (ifブロック専用)
    uint8_t type;    // 2=block, 3=loop, 4=if
} Block;

typedef struct {
    uint8_t *code;
    size_t size;
    size_t pc;

    int32_t stack[256];
    int sp;

    int32_t locals[16];

    Block block_stack[64];
    int block_sp;

    CallFrame call_stack[64];
    int call_sp;

    char string_buffer[4096];
    size_t string_buffer_ptr;

    uint8_t memory[65536]; // 線形メモリ (64KB)
    uint32_t memory_pages;   // 確保されているメモリのページ数

    ImportFunc import_funcs[MAX_IMPORT_FUNCS]; // Wasmモジュールが要求するインポート
    size_t import_func_count;

    FuncType func_types[64];
    size_t func_type_count;

    ExportFunc export_funcs[MAX_EXPORT_FUNCS];
    size_t export_func_count;

    MemoryExport memory_exports[1]; // メモリのエクスポートは1つまで
    size_t memory_export_count;

    size_t func_count;       // module 内関数数
    size_t func_pcs[256];    // index → code 上の PC
    uint32_t func_type_indices[256]; // index -> type_index
} WasmVM;

// ホスト関数をVMに登録する。Wasmモジュールのインポートと名前でマッチングする。
void vm_register_import(WasmVM *vm, const char *mod_name, const char *field_name, ImportFuncPtr func) {
    for (size_t i = 0; i < vm->import_func_count; i++) {
        if (strcmp(vm->import_funcs[i].mod_name, mod_name) == 0 &&
            strcmp(vm->import_funcs[i].field_name, field_name) == 0) {
            vm->import_funcs[i].func = func;
            return;
        }
    }
}

// VMの内部バッファに文字列を追加し、そのポインタを返す
char *add_string_to_buffer(WasmVM *vm, const char *str) {
    size_t len = strlen(str);
    if (vm->string_buffer_ptr + len + 1 > sizeof(vm->string_buffer)) {
        printf("String buffer overflow\n");
        return NULL;
    }
    strcpy(vm->string_buffer + vm->string_buffer_ptr, str);
    char *ptr = vm->string_buffer + vm->string_buffer_ptr;
    vm->string_buffer_ptr += len + 1;
    return ptr;
}

// LEB128デコード関数
uint32_t read_uLEB128(uint8_t *buf, size_t *pc) {
    uint32_t result = 0;
    int shift = 0;
    uint8_t byte;
    while (1) {
        byte = buf[(*pc)++];
        result |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

int32_t read_sLEB128(uint8_t *buf, size_t *pc) {
    int32_t result = 0;
    int shift = 0;
    uint8_t byte;
    const int nbits = 32;
    while (1) {
        byte = buf[(*pc)++];
        result |= (int32_t)(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            if (shift < nbits && (byte & 0x40)) {
                result |= - (1 << shift);
            }
            break;
        }
    }
    return result;
}

void parse_type_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t type_count = read_uLEB128(vm->code, pc);
printf("  type_count=%u\n", type_count);
    for (uint32_t i = 0; i < type_count; i++) {
        uint8_t form = vm->code[(*pc)++]; // 0x60 for func
        if (form != 0x60) continue;

        FuncType ftype = {0};

        // パラメータ
        ftype.param_count = read_uLEB128(vm->code, pc);
printf("    type[%u]: params=%d, ", i, ftype.param_count);
        for (int j = 0; j < ftype.param_count; j++) {
            ftype.param_types[j] = vm->code[(*pc)++];
        }

        // 戻り値
        ftype.result_count = read_uLEB128(vm->code, pc);
printf("results=%d\n", ftype.result_count);
        for (int j = 0; j < ftype.result_count; j++) {
            ftype.result_types[j] = vm->code[(*pc)++];
        }

        if (vm->func_type_count < 64) {
            vm->func_types[vm->func_type_count++] = ftype;
        }
    }
}

void parse_import_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t import_count = read_uLEB128(vm->code, pc);
    printf("  import_count=%d\n", import_count);
    for (uint32_t i = 0; i < import_count; i++) {
        uint32_t mlen = read_uLEB128(vm->code, pc);
        char mod_name[256];
        if (mlen < sizeof(mod_name)) {
            memcpy(mod_name, (char*)(vm->code + *pc), mlen);
            mod_name[mlen] = '\0';
        }
        *pc += mlen;

        uint32_t flen = read_uLEB128(vm->code, pc);
        char field_name[256];
        if (flen < sizeof(field_name)) {
            memcpy(field_name, (char*)(vm->code + *pc), flen);
            field_name[flen] = '\0';
        }
        *pc += flen;

        uint8_t kind = vm->code[(*pc)++];
        printf("  import[%d]: mod='%s', field='%s', kind=%d\n", i, mod_name, field_name, kind);
        if (kind == 0x00) { // function import
            uint32_t type_index = read_uLEB128(vm->code, pc);
            printf("    type_index=%d\n", type_index);
            if (vm->import_func_count < MAX_IMPORT_FUNCS) {
                vm->import_funcs[vm->import_func_count++] = (ImportFunc){ add_string_to_buffer(vm, mod_name), add_string_to_buffer(vm, field_name), type_index, 0, NULL };
            }
        } else if (kind == 0x02) { // memory import
            // メモリインポートのパース (現在はスキップするだけ)
            uint8_t flags = vm->code[(*pc)++];
            (void)read_uLEB128(vm->code, pc); // initial pages
            if (flags & 0x01) {
                (void)read_uLEB128(vm->code, pc); // max pages
            }
        } else { /* other imports */ }
    }
}

void parse_function_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t func_count = read_uLEB128(vm->code, pc);
    printf("  function_count=%u\n", func_count);
    vm->func_count = vm->import_func_count + func_count;
    for (uint32_t i = 0; i < func_count; i++) {
        uint32_t type_index = read_uLEB128(vm->code, pc);
        size_t func_idx = vm->import_func_count + i;
        printf("    func[%zu] has type_index %u\n", func_idx, type_index);
        if (func_idx < 256) {
            vm->func_type_indices[func_idx] = type_index;
        }
    }
}

void parse_export_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t export_count = read_uLEB128(vm->code, pc);
printf("  export_count=%u\n", export_count);
    for (uint32_t i = 0; i < export_count; i++) {
        uint32_t nlen = read_uLEB128(vm->code, pc);
        char name[256];
        if (nlen < sizeof(name)) {
            memcpy(name, (char *)(vm->code + *pc), nlen);
            name[nlen] = '\0';
        }
        *pc += nlen;
        uint8_t kind = vm->code[(*pc)++];
        uint32_t index = read_uLEB128(vm->code, pc);
printf("  export[%u]: name='%s', kind=%u, index=%u\n", i, name, kind, index);
        if (kind == 0x00) { // function export
            if (vm->export_func_count < MAX_EXPORT_FUNCS) {
                vm->export_funcs[vm->export_func_count++] = (ExportFunc){ add_string_to_buffer(vm, name), index, 0 };
            }
        } else if (kind == 0x02) { // memory export
            if (vm->memory_export_count < 1) {
                vm->memory_exports[vm->memory_export_count++] = (MemoryExport){ add_string_to_buffer(vm, name), index };
            }
        } else {
            // 他のエクスポート種別は未サポート
        }
    }
}

void parse_memory_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t count = read_uLEB128(vm->code, pc);
    printf("  memory_count=%u\n", count);
    for (uint32_t i = 0; i < count; i++) {
        // 1つ目のメモリ定義のみサポート
        uint8_t flags = vm->code[(*pc)++];
        if (flags & 0x80) { // export flag
            uint32_t nlen = read_uLEB128(vm->code, pc);
            char name[256];
            if (nlen < sizeof(name)) {
                memcpy(name, (char *)(vm->code + *pc), nlen);
                name[nlen] = '\0';
            }
            *pc += nlen;
            printf("    memory[%u] is exported as '%s'\n", i, name);
            if (vm->memory_export_count < 1) {
                vm->memory_exports[vm->memory_export_count++] = (MemoryExport){ add_string_to_buffer(vm, name), i };
            }
        }
        uint32_t initial_pages = read_uLEB128(vm->code, pc);
        vm->memory_pages = initial_pages;
        printf("    memory[0]: initial_pages=%u", initial_pages);
        if (flags & 0x01) { // max指定あり
            uint32_t max_pages = read_uLEB128(vm->code, pc);
            printf(", max_pages=%u\n", max_pages);
        } else {
            printf("\n");
        }
        // 現在の実装ではメモリは64KB固定なので、この値は情報として保持するのみ
    }
}

void parse_data_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t count = read_uLEB128(vm->code, pc);
    printf("  data_segment_count=%u\n", count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t mem_idx = read_uLEB128(vm->code, pc); // 0x00のはず
        (void)mem_idx;
        // オフセット式 (i32.const + end)
        uint8_t op = vm->code[(*pc)++];
        (void)op;
        int32_t offset = read_sLEB128(vm->code, pc);
        (*pc)++; // end opcode
        uint32_t data_size = read_uLEB128(vm->code, pc);
        printf("    data[%u]: offset=%d, size=%u\n", i, offset, data_size);
        memcpy(vm->memory + offset, vm->code + *pc, data_size);
        // --- DEBUG PRINT ---
        printf("      data content written to memory: \"");
        for(uint32_t j=0; j<data_size; j++) {
            printf("%c", vm->memory[offset+j]);
        }
        printf("\"\n");
        // --- END DEBUG PRINT ---
        *pc += data_size;
    }
}

void parse_code_section(WasmVM *vm, size_t *pc, size_t end_pc) {
    uint32_t func_count = read_uLEB128(vm->code, pc);
    printf("  code_body_count=%u\n", func_count);
    for (uint32_t i = 0; i < func_count; i++) {
        uint32_t body_size = read_uLEB128(vm->code, pc);
        size_t func_start_pc = *pc;
        size_t func_idx = vm->import_func_count + i;
        printf("    body[%u] (func_idx %zu): size=%u, start_pc=%zu\n", i, func_idx, body_size, func_start_pc);
        if (func_idx < 256) {
            vm->func_pcs[vm->import_func_count + i] = func_start_pc;
        }
        *pc += body_size;
    }
}

void parse_sections(WasmVM *vm) {
    size_t pc = 8; // magic + version
    while (pc < vm->size) {
        uint8_t sec_id = vm->code[pc++];
        uint32_t sec_size = read_uLEB128(vm->code, &pc);
        size_t next_sec_start = pc + sec_size;
printf("sec_id=%d, sec_size=%d, pc=%zu, next_pc=%zu\n", sec_id, sec_size, pc, next_sec_start);
        switch (sec_id) {
            case 1: // Type Section
                parse_type_section(vm, &pc, next_sec_start);
                break;
            case 2: // Import Section
                parse_import_section(vm, &pc, next_sec_start);
                break;
            case 3: // Function Section
                parse_function_section(vm, &pc, next_sec_start);
                break;
            case 5: // Memory Section
                parse_memory_section(vm, &pc, next_sec_start);
                break;
            case 7: // Export Section
                parse_export_section(vm, &pc, next_sec_start);
                break;
            case 10: // Code Section
                parse_code_section(vm, &pc, next_sec_start);
                break;
            case 11: // Data Section
                parse_data_section(vm, &pc, next_sec_start);
                break;
            default: // 未知または未実装のセクションはスキップ
                pc = next_sec_start; // 次のセクションの開始位置にpcを正しく設定
                break;
        }
    }
}

// import関数をモジュール名＋フィールド名で検索
ImportFunc *find_import(WasmVM *vm, const char *mod, const char *field) {
    for (size_t i = 0; i < vm->import_func_count; i++) {
        ImportFunc *f = &vm->import_funcs[i];
        if (strcmp(f->mod_name, mod) == 0 && strcmp(f->field_name, field) == 0)
            return f;
    }
    return NULL;
}

// export関数を名前で検索
ExportFunc *find_export(WasmVM *vm, const char *name) {
    for (size_t i = 0; i < vm->export_func_count; i++) {
        ExportFunc *f = &vm->export_funcs[i];
        if (strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

// 簡易的に WebAssembly の命令のオペランド長を判定してスキップする関数
size_t skip_operands(uint8_t op, uint8_t *code, size_t pc) {
    switch (op) {
        case 0x20: // local.get
        case 0x21: // local.set
        case 0x22: // local.tee
        case 0x23: // global.get
        case 0x24: // global.set
        case 0x10: // call
            (void)read_uLEB128(code, &pc);
            break;
        case 0x41: // i32.const
        case 0x42: // i64.const
            (void)read_sLEB128(code, &pc);
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B: // load
        case 0x36: case 0x37: case 0x38: case 0x39: // store
            (void)read_uLEB128(code, &pc); // align
            (void)read_uLEB128(code, &pc); // offset
            break;
        default:
            // その他の命令はオペランドなし
            break;
    }
    return pc;
}

// start_pc の if/block/loop の対応する end_pc を返す
size_t find_structured_end(uint8_t *code, size_t code_size, size_t start_pc, size_t *else_pc_out) {
    int depth = 1;
    size_t pc = start_pc;
    if (else_pc_out) *else_pc_out = 0;

    while (depth > 0 && pc < code_size) {
        uint8_t op = code[pc++];
        switch (op) {
            case 0x02: // block
            case 0x03: // loop
            case 0x04: // if
                depth++;
                pc++; // skip blocktype
                break;
            case 0x05: // else
                if (depth == 1 && else_pc_out) *else_pc_out = pc;
                break;
            case 0x0B: // end
                depth--;
                if (depth == 0) return pc; // end_pc を返す
                break;
            default:
                pc = skip_operands(op, code, pc);
                break;
        }
    }
    return pc;
}

void run(WasmVM *vm) {
    while (1) {
        size_t current_pc = vm->pc; // 実行前のpcを保存
        if (current_pc >= vm->size) {
            printf("PC out of bounds\n");
            return;
        }
        uint8_t op = vm->code[vm->pc++]; // 命令を読み込み、pcをインクリメント
        printf("opcode: 0x%02X at pc=%zu; ", op, current_pc);
        switch (op) {
            case 0x20: { // local.get
                uint32_t i = read_uLEB128(vm->code, &vm->pc);
                vm->stack[vm->sp++] = vm->locals[i];
                printf("[local.get] %d: %d\n", i, vm->locals[i]);
                break;
            }
            case 0x21: { // local.set
                uint32_t i = read_uLEB128(vm->code, &vm->pc);
                vm->locals[i] = vm->stack[--vm->sp];
                break;
            }
            case 0x22: { // local.tee
                uint32_t i = read_uLEB128(vm->code, &vm->pc);  // ローカルインデックスを取得
                vm->locals[i] = vm->stack[vm->sp - 1];        // スタックトップの値をローカルにコピー
                // vm->sp は減らさない → スタックに値を残す
                break;
            }

            case 0x28: { // i32.load
                (void)read_uLEB128(vm->code, &vm->pc); // align
                uint32_t offset = read_uLEB128(vm->code, &vm->pc);
                uint32_t addr = (uint32_t)vm->stack[--vm->sp] + offset;
                if (addr + 4 > sizeof(vm->memory)) { printf("Memory load out of range\n"); return; }
                int32_t val = (int32_t)(
                    vm->memory[addr] |
                    (vm->memory[addr + 1] << 8) |
                    (vm->memory[addr + 2] << 16) |
                    (vm->memory[addr + 3] << 24)
                );
                vm->stack[vm->sp++] = val;
                break;
            }

            case 0x36: { // i32.store
                (void)read_uLEB128(vm->code, &vm->pc); // align
                uint32_t offset = read_uLEB128(vm->code, &vm->pc);
                int32_t val = vm->stack[--vm->sp];
                uint32_t addr = (uint32_t)vm->stack[--vm->sp] + offset;
                if (addr + 4 > sizeof(vm->memory)) { printf("Memory store out of range\n"); return; }
                printf("[i32.store] addr=%u, val=%d (offset=%u) ", addr, val, offset);
                vm->memory[addr]     = val & 0xFF;
                vm->memory[addr + 1] = (val >> 8) & 0xFF;
                vm->memory[addr + 2] = (val >> 16) & 0xFF;
                vm->memory[addr + 3] = (val >> 24) & 0xFF;
                uint32_t written_val = (uint32_t)(
                    vm->memory[addr] |
                    (vm->memory[addr + 1] << 8) |
                    (vm->memory[addr + 2] << 16) |
                    (vm->memory[addr + 3] << 24)
                );
                printf(" -> Verifying memory at addr=%u: read back value is %u\n", addr, written_val);
                break;
            }

            case 0x41: { // i32.const
                int32_t val = read_sLEB128(vm->code, &vm->pc);
                printf("[i32.const] %d\n", val);
                vm->stack[vm->sp++] = val;
                break;
            }

            case 0x67: { // i32.clz
                int32_t value = vm->stack[--vm->sp];
                if (value == 0) {
                    vm->stack[vm->sp++] = 32;  // 全部0なら leading zeros = 32
                    break;
                }
            
                uint32_t uvalue = (uint32_t)value;
                int32_t count = 0;
            
                // 上位ビットから順に0を数える
                for (int i = 31; i >= 0; i--) {
                    if (uvalue & (1u << i)) break; // 最初に1が見つかったら終了
                    count++;
                }
            
                vm->stack[vm->sp++] = count;
                break;
            }
            case 0x68: { // i32.ctz
                int32_t value = vm->stack[--vm->sp];  // スタックから値を取り出す
                if (value == 0) {
                    vm->stack[vm->sp++] = 32;       // 入力が0なら32ビット分すべてゼロ
                    break;
                }
            
                int32_t count = 0;
                uint32_t uvalue = (uint32_t)value;   // 符号を無視してビット演算
                while ((uvalue & 1) == 0) {           // 末尾のゼロを数える
                    count++;
                    uvalue >>= 1;
                }
                vm->stack[vm->sp++] = count;          // スタックに結果を戻す
                break;
            }

            case 0x6A: { // i32.add
                int32_t b = vm->stack[--vm->sp];
                int32_t a = vm->stack[--vm->sp];
                vm->stack[vm->sp++] = a + b;
                break;
            }
            case 0x6B: { // i32.sub
                int32_t b = vm->stack[--vm->sp];
                int32_t a = vm->stack[--vm->sp];
                printf("[i32.sub] a = %d, b = %d\n", a, b);
                vm->stack[vm->sp++] = a - b;
                break;
            }
            case 0x6C: { // i32.mul
                int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = a * b; break;
            }
            case 0x6D: { // i32.div_s
                int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; if (b == 0) { return; } if (a == INT32_MIN && b == -1) { return; }
                vm->stack[vm->sp++] = a / b;
                break;
            }
            case 0x6E: { // i32.div_u
                uint32_t b = vm->stack[--vm->sp]; uint32_t a = vm->stack[--vm->sp]; if (b == 0) { return; }
                vm->stack[vm->sp++] = (int32_t)(a / b);
                break;
            }
            case 0x6F: { // i32.rem_s
                int32_t b = vm->stack[--vm->sp];
                int32_t a = vm->stack[--vm->sp];
                if (b == 0) { return; }
                vm->stack[vm->sp++] = a % b;
                break;
            }
            case 0x70: { // i32.rem_u
                uint32_t b = (uint32_t)vm->stack[--vm->sp];
                uint32_t a = (uint32_t)vm->stack[--vm->sp];
                if (b == 0) { return; }
                vm->stack[vm->sp++] = a % b;
                break;
            }

            case 0x45: { int32_t v = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (v == 0); break; }
            case 0x48: { int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a < b); break; }
            case 0x49: { uint32_t b = vm->stack[--vm->sp]; uint32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a < b); break; }
            case 0x4A: { int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a > b); break; }
            case 0x4B: { uint32_t b = vm->stack[--vm->sp]; uint32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a > b); break; }
            case 0x4C: { int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a <= b); break; }
            case 0x4D: { // le_u
                uint32_t b = vm->stack[--vm->sp];
                uint32_t a = vm->stack[--vm->sp];
                printf("[i32.le_u] a = %d, b = %d\n", a, b);
                vm->stack[vm->sp++] = (a <= b);
                break;
            }
            case 0x4E: { int32_t b = vm->stack[--vm->sp]; int32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a >= b); break; }
            case 0x4F: { uint32_t b = vm->stack[--vm->sp]; uint32_t a = vm->stack[--vm->sp]; vm->stack[vm->sp++] = (a >= b); break; }

            case 0x01: { // nop
                printf("[nop]\n");
                break;
            }
            case 0x02: { // block
                vm->pc++;
                size_t end = find_structured_end(vm->code, vm->size, vm->pc, NULL);
                vm->block_stack[vm->block_sp++] = (Block){.start_pc=vm->pc, .end_pc=end, .type=2};
                break;
            }
            case 0x03: { // loop
                vm->pc++;
                size_t start = vm->pc;
                size_t end = find_structured_end(vm->code, vm->size, vm->pc, NULL);
                vm->block_stack[vm->block_sp++] = (Block){.start_pc=start, .end_pc=end, .type=3};
                break;
            }

            case 0x04: { // if
                vm->pc++; // blocktype をスキップ
                size_t then_start_pc = vm->pc;
                size_t else_pc;
                size_t end_pc = find_structured_end(vm->code, vm->size, vm->pc, &else_pc);
                printf("[if] else_pc = %ld, end_pc = %ld; ", else_pc, end_pc);
                int32_t cond = vm->stack[--vm->sp];
                printf("cond = %d\n", cond);
                
                vm->block_stack[vm->block_sp++] = (Block){.start_pc=then_start_pc, .end_pc=end_pc, .else_pc=else_pc, .type=4};
                
                if (cond == 0) {
                    if (else_pc != 0) vm->pc = else_pc;
                    else vm->pc = end_pc;
                }
                break;
            }

            case 0x05: { // else
                // if(cond==true) の場合に thenブロックの終端から実行される
                printf("[else] (pc=%zu) ", current_pc);
                if (vm->block_sp > 0) {
                    Block current_block = vm->block_stack[vm->block_sp - 1];
                    // thenブロックを実行したので、対応するendまでジャンプする
                    if (current_block.type == 4) {
                        printf("Jumping from 'else' block. Target end_pc is %zu; ", current_block.end_pc);
                        vm->pc = current_block.end_pc;
                        printf("Next loop iteration will execute from pc=%zu.\n", vm->pc);
                    } else {
                        printf("current_block.type=%d\n", current_block.type);
                    }
                } else {
                    printf("No block on stack for else. \n");
                }
                break;
            }

            case 0x0B: { // end
                printf("[end] pc=%zu. block_sp=%d, call_sp=%d\n", current_pc, vm->block_sp, vm->call_sp);
            
                // 現在のPCがブロックの終端を超えた場合も含めてポップ
                while (vm->block_sp > 0 && vm->block_stack[vm->block_sp - 1].end_pc <= current_pc) {
                    Block ended_block = vm->block_stack[--vm->block_sp];
                    printf("  -> Block end. Popped block. New block_sp=%d. Block type=%d\n", vm->block_sp, ended_block.type);
                }
            
                // ブロックスタックが空になったら関数の終端
                if (vm->block_sp == 0) {
                    printf("  -> Function end.\n");
                    if (vm->call_sp > 0) {
                        CallFrame frame = vm->call_stack[--vm->call_sp];
                        memcpy(vm->locals, frame.locals, sizeof(vm->locals));
                        vm->pc = frame.return_pc;
                        printf("    [return from function] -> Set pc to %zu, call_sp=%d. Restored locals[0]=%d\n",
                               vm->pc, vm->call_sp, vm->locals[0]);
                    } else {
                        printf("    [return from top level]. Final sp=%d\n", vm->sp);
                        return;
                    }
                }
                break;
            }

            case 0x0C: { // br
                printf("[br]\n");
                uint32_t depth = read_uLEB128(vm->code, &vm->pc);
                if (depth >= vm->block_sp) { return; }
                Block target = vm->block_stack[vm->block_sp - 1 - depth];
                if (target.type == 3) { vm->pc = target.start_pc; } 
                else { vm->block_sp -= (depth + 1); vm->pc = target.end_pc; }
                break;
            }

            case 0x0D: { // br_if
                printf("[br if]\n");
                uint32_t depth = read_uLEB128(vm->code, &vm->pc);
                if (vm->stack[--vm->sp] != 0) {
                    if (depth >= vm->block_sp) { return; }
                    Block target = vm->block_stack[vm->block_sp - 1 - depth];
                    if (target.type == 3) { vm->pc = target.start_pc; }
                    else { vm->block_sp -= (depth + 1); vm->pc = target.end_pc; }
                }
                break;
            }

            case 0x0F: { // return
                printf("return; (at pc=%zu) ", current_pc);
                        printf("\n--- DEBUG: Returning from fib(1) ---\n");
                        printf("    Return value on stack: %d, locals[0]: %d\n\n", vm->stack[vm->sp-1], vm->locals[0]);
                if (vm->call_sp > 0) {
                    CallFrame frame = vm->call_stack[--vm->call_sp];
                    // --- ADD: ローカル変数をCallFrameから復元 ---
                    memcpy(vm->locals, frame.locals, sizeof(vm->locals));
                    vm->pc = frame.return_pc;
                    printf("  [return from function] -> Set pc to %zu, call_sp=%d. Restored locals[0] = %d\n", vm->pc, vm->call_sp, vm->locals[0]);
                } else {
                    return;
                }
                break;
            }

            case 0x10: { // call
                printf("[call] pc=%zu. block_sp=%d, call_sp=%d; ", current_pc, vm->block_sp, vm->call_sp);
                uint32_t idx = read_uLEB128(vm->code, &vm->pc);

                if (idx < vm->import_func_count) {
                    // --- インポート関数の呼び出し ---
                    ImportFunc *f = &vm->import_funcs[idx];
                    if (f->func == NULL) {
                        printf("Unresolved import function: %s.%s\n", f->mod_name, f->field_name);
                        return;
                    }
                    FuncType *ftype = &vm->func_types[f->type_index];
                    int param_count = ftype->param_count;

                    printf("{call import} func_idx=%u, name='%s.%s', params=%d\n", idx, f->mod_name, f->field_name, param_count);
                    vm->sp -= param_count;
                    int32_t ret = f->func(&vm->stack[vm->sp], param_count);

                    if (ftype->result_count > 0) {
                        vm->stack[vm->sp++] = ret;
                    }
                } else {
                    // --- 内部関数の呼び出し ---
                    uint32_t type_idx = vm->func_type_indices[idx];
                    FuncType *ftype = &vm->func_types[type_idx];
                    int param_count = ftype->param_count;

                    printf("{call internal} func_idx=%u, type_idx=%u, params=%d, vm->call_sp=%d; ", idx, type_idx, param_count, vm->call_sp);

                    // 関数呼び出しスタックに現在の状態を保存
                    if (vm->call_sp >= 64) { printf("Call stack overflow\n"); return; }
                    // --- ADD: ローカル変数をCallFrameに保存 ---
                    printf("Current locals[0] = %d; ", vm->locals[0]);
                    memcpy(vm->call_stack[vm->call_sp].locals, vm->locals, sizeof(vm->locals));
                    printf("Saved locals[0] = %d; ", vm->call_stack[vm->call_sp].locals[0]);
                    // vm->call_stack[vm->call_sp++] = (CallFrame){ .return_pc = vm->pc };
                    vm->call_stack[vm->call_sp++].return_pc = vm->pc;

                    // 新しい関数のPCにジャンプ
                    vm->pc = vm->func_pcs[idx];

                    // --- 関数のプロローグ (runの先頭にあった処理をここに移動) ---
                    // スタックから引数をローカル変数にコピー
                    for (int i = param_count - 1; i >= 0; i--) {
                        vm->locals[i] = vm->stack[--vm->sp];
                    }
                    // デバッグ出力
                    for (int i = 0; i < param_count; i++) {
                        printf("arg[%d] = %d; ", i, vm->locals[i]);
                    }
                    printf("\n");
                    // ローカル変数宣言をパース
                    uint32_t local_groups = read_uLEB128(vm->code, &vm->pc);
                    for (uint32_t i = 0; i < local_groups; i++) {
                        (void)read_uLEB128(vm->code, &vm->pc); // num_locals
                        (void)vm->code[vm->pc++]; // type
                    }
                    // --- 関数のプロローグここまで ---
                }
                break;
            }

            case 0x1A: { // drop
                printf("[drop]\n");
                vm->sp--;
                break;
            }

            default:
                printf("Unknown or unimplemented opcode: 0x%02X at pc=%zu\n", op, vm->pc - 1);
                return; // ここで終了
        }
    }
}

int32_t print_i32(int32_t *args, int argc __attribute__((unused))) {
    printf("print_i32: %d\n", args[0]);
    return 0;
}

int32_t imported_add(int32_t *args, int argc) {
    if (argc != 2) return -1;
printf("imported_add\n");
    return args[0] + args[1];
}

// WASIのfd_writeをシミュレートするホスト関数
int32_t wasi_fd_write(int32_t *args, int argc) {
    if (argc != 4) return -1; // __WASI_ERRNO_INVAL

    int32_t fd = args[0];
    int32_t iovs_ptr = args[1];
    int32_t iovs_len = args[2];
    int32_t nwritten_ptr = args[3];

    // --- DEBUG PRINT ---
    printf("  [wasi_fd_write called]\n");
    printf("    fd: %d, iovs_ptr: %d, iovs_len: %d, nwritten_ptr: %d\n", fd, iovs_ptr, iovs_len, nwritten_ptr);
    // --- END DEBUG PRINT ---

    WasmVM *original_vm_ptr = (WasmVM*)((char*)args - offsetof(WasmVM, stack));
    // --- DEBUG PRINT ---
    // This is just for debugging. We know this pointer is likely incorrect.
    // We will compare it with a hypothetically correct pointer if we had one.
    printf("      -> Reconstructed vm pointer: %p\n", (void*)original_vm_ptr);
    // --- END DEBUG PRINT ---
    WasmVM *vm = original_vm_ptr;

    if (fd != 1) { // stdout以外は未サポート
        return -1; // __WASI_ERRNO_BADF
    }

    uint32_t bytes_written = 0;
    for (int i = 0; i < iovs_len; i++) {
        uint32_t iov_base = *(uint32_t*)&vm->memory[iovs_ptr + i * 8];
        uint32_t iov_len = *(uint32_t*)&vm->memory[iovs_ptr + i * 8 + 4];
        // --- DEBUG PRINT ---
        printf("    iov[%d]: base=%u, len=%u, content=\"", i, iov_base, iov_len);
        fwrite(&vm->memory[iov_base], 1, iov_len, stdout);
        printf("\"\n");
        // --- END DEBUG PRINT ---
        fwrite(&vm->memory[iov_base], 1, iov_len, stdout);
        bytes_written += iov_len;
    }

    *(uint32_t*)&vm->memory[nwritten_ptr] = bytes_written;
    return 0; // __WASI_ERRNO_SUCCESS
}

// Wasmバイナリを16進数でダンプする関数
void dump_wasm_code(const uint8_t *code, size_t size) {
    printf("--- Wasm Code Dump (size: %zu bytes) ---\n", size);
    for (size_t i = 0; i < size; i += 16) {
        // オフセット表示
        printf("%08zx: ", i);

        // 16進数表示
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) {
                printf("%02x ", code[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) {
                printf(" ");
            }
        }
        printf(" |");

        // ASCII表示
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) {
                uint8_t c = code[i + j];
                printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
        }
        printf("|\n");
    }
    printf("----------------------------------------\n");
}

// ファイルからWasmバイナリを読み込む関数
// 成功した場合、bufferに確保したメモリのポインタ、sizeにファイルサイズを格納し、0を返す
// 失敗した場合、-1を返す
int read_wasm_file(const char *filepath, uint8_t **buffer, size_t *size) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("Failed to open wasm file");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);

    *buffer = (uint8_t *)malloc(*size);
    if (!*buffer) {
        fprintf(stderr, "Failed to allocate memory for wasm file\n");
        fclose(file);
        return -1;
    }

    if (fread(*buffer, 1, *size, file) != *size) {
        fprintf(stderr, "Failed to read wasm file\n");
        fclose(file);
        free(*buffer);
        return -1;
    }

    fclose(file);
    return 0;
}

int main() {
    // --- テストケース1: 基本的な演算とローカル変数 ---
    uint8_t code[] = {
        0x41, 0x05,       // i32.const 5      ; 定数 5 をスタックに積む
        0x21, 0x00,       // local.set 0      ; スタックの値をローカル変数0に格納
        0x41, 0x07,       // i32.const 7      ; 定数 7 をスタックに積む
        0x21, 0x01,       // local.set 1      ; スタックの値をローカル変数1に格納
        0x20, 0x00,       // local.get 0      ; ローカル変数0の値をスタックに積む
        0x20, 0x01,       // local.get 1      ; ローカル変数1の値をスタックに積む
        0x6A,             // i32.add          ; スタックの2つの値を加算
        0x21, 0x02,       // local.set 2      ; 結果をローカル変数2に格納
        0x0B              // end
    };
    WasmVM vm;
    memset(&vm, 0, sizeof(vm)); vm.code = code; vm.size = sizeof(code);
    vm.pc = 0;
    run(&vm);
    printf("locals[2] = %d (expected 12)\n", vm.locals[2]);
    printf("--------------------\n");

    // --- テストケース2: 除算 ---
    uint8_t code1[] = {
        0x41, 0x0A,       // i32.const 10    ; 定数 10 をスタックに積む
        0x41, 0x02,       // i32.const 2     ; 定数 2 をスタックに積む
        0x6D,             // i32.div_s       ; スタックの2つの値を符号付き整数で割る (10 / 2)
        0x0B              // end
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code1; vm.size = sizeof(code1);
    vm.pc = 0;
    run(&vm);
    printf("10 / 2 = %d (expected 5)\n", vm.stack[0]);
    printf("--------------------\n");

    // --- テストケース2.1 ---
    uint8_t code_ctz[] = {
        0x41, 0x80, 0x80, 0x80, 0x04, // i32.const 8388608
        0x68,                         // i32.ctz
        0x0B                          // end
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code_ctz; vm.size = sizeof(code_ctz);
    vm.pc = 0;
    run(&vm);
    printf("ctz 8388608 = %d (expected 23)\n", vm.stack[0]);
    printf("--------------------\n");

    // --- テストケース2.2 ---
    uint8_t code_clz[] = {
        0x41, 0x80, 0x80, 0x80, 0x04, // i32.const 8388608
        0x67,                         // i32.clz
        0x0B                          // end
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code_clz; vm.size = sizeof(code_clz);
    vm.pc = 0;
    run(&vm);
    printf("clz 8388608 = %d (expected 8)\n", vm.stack[0]);
    printf("--------------------\n");

    // --- テストケース3: 負数の除算 ---
    uint8_t code2[] = {
        0x41, 0x7F,       // i32.const -1   ; 定数 -1 をスタックに積む（0x7F は 2の補数で -1）
        0x41, 0x01,       // i32.const 1    ; 定数 1 をスタックに積む
        0x6D,             // i32.div_s      ; スタックの2つの値を符号付き整数で割る (-1 / 1)
        0x0B
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code2; vm.size = sizeof(code2);
    vm.pc = 0;
    run(&vm);
    printf("-1 / 1 = %d (expected -1)\n", vm.stack[0]);
    printf("--------------------\n");

    // --- テストケース4: ループ (0から4の合計を計算) ---
    uint8_t code_loop[] = {
        0x41, 0x00, 0x21, 0x00,       // i32.const 0; local.set 0  → i = 0
        0x41, 0x00, 0x21, 0x01,       // i32.const 0; local.set 1  → sum = 0
        0x02, 0x40,                    // block (外側ブロック)
            0x03, 0x40,                // loop (ループ開始)
                0x20, 0x00,            // local.get 0  → i
                0x41, 0x05,            // i32.const 5
                0x4E,                  // i32.ge_s     → i >= 5 ?
                0x0D, 0x01,            // br_if 1      → 外側ブロックにジャンプしてループ終了
                0x20, 0x01,            // local.get 1  → sum
                0x20, 0x00,            // local.get 0  → i
                0x6A,                  // i32.add      → sum + i
                0x21, 0x01,            // local.set 1  → sum に保存
                0x20, 0x00,            // local.get 0  → i
                0x41, 0x01,            // i32.const 1
                0x6A,                  // i32.add      → i + 1
                0x21, 0x00,            // local.set 0  → i に保存
                0x0C, 0x00,            // br 0         → loop の先頭に戻る
            0x0B,
        0x0B                         // end (外側ブロック終了)
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code_loop; vm.size = sizeof(code_loop);
    vm.pc = 0;
    vm.block_stack[vm.block_sp++] = (Block){.end_pc = vm.size, .type = 2};
    run(&vm);
    printf("sum(0..4) = %d (expected 10)\n", vm.locals[1]);
    printf("--------------------\n");

    // --- テストケース5: if/else ---
    uint8_t code_if[] = {
        0x20, 0x00,                 // local.get
        0x45,                       // i32.eqz
        0x04, 0x40,                 // if (void)
        0x41, 0xef, 0x00,           // i32.const 111
        0x05,                       // else
        0x41, 0xde, 0x01,           // i32.const 222
        0x0B
    };
    
    // param = 0 のとき (cond=true)
    memset(&vm, 0, sizeof(vm)); vm.code = code_if; vm.size = sizeof(code_if); vm.locals[0] = 0;
    vm.pc = 0;
    run(&vm);
    printf("if (0==0) result = %d (expected 111)\n", vm.stack[vm.sp-1]);
    
    // param = 1 のとき (cond=false)
    memset(&vm, 0, sizeof(vm)); vm.code = code_if; vm.size = sizeof(code_if); vm.locals[0] = 1;
    vm.pc = 0;
    run(&vm);
    printf("if (1==0) result = %d (expected 222)\n", vm.stack[vm.sp-1]);
    printf("--------------------\n");

    // --- テストケース6: 線形メモリの read/write ---
    uint8_t code_mem[] = {
        0x41, 0x00,             // i32.const 0   -> addr = 0
        0x41, 0xF8, 0x00,       // i32.const 120 -> val = 120 (sLEB128: 0xF8 0x00)
        0x36, 0x02, 0x00,       // i32.store align=2 offset=0
    
        0x41, 0x00,             // i32.const 0   -> addr = 0
        0x28, 0x02, 0x00,       // i32.load  align=2 offset=0
        0x0B
    };
    memset(&vm, 0, sizeof(vm)); vm.code = code_mem; vm.size = sizeof(code_mem);
    vm.pc = 0;
    run(&vm);
    printf("memory[0] loaded = %d (expected 120)\n", vm.stack[vm.sp-1]);
    printf("--------------------\n");

    // --- テストケース7: 型セクション、インポート/エクスポートを含むWasmモジュール ---
    printf("--- Test Case 7: Full module parsing and execution ---\n");
    uint8_t wasm_module[] = {
        // Magic + Version
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // Section 1: Type
        0x01, 0x0c, 0x02, // Section size 12, 2 types
        // type 0: (i32, i32) -> i32 (for imported_add)
        0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
        // type 1: (i32) -> () (for print_i32)
        0x60, 0x01, 0x7f, 0x00,
        // Section 2: Import
        0x02, 0x19, 0x02, // Section size 25, 2 imports
        // import 0: "env"."add" (type 0)
        0x03, 'e', 'n', 'v', 0x03, 'a', 'd', 'd', 0x00, 0x00,
        // import 1: "env"."print_i32" (type 1)
        0x03, 'e', 'n', 'v', 0x09, 'p', 'r', 'i', 'n', 't', '_', 'i', '3', '2', 0x00, 0x01,
        // Section 3: Function
        0x03, 0x02, 0x01, 0x02, // Section size 2, 1 function, type_idx 2
        // Section 7: Export
        0x07, 0x0b, 0x01, // Section size 11, 1 export
        // export "main_add" -> func_idx 2 (0:import, 1:import, 2:internal)
        0x08, 'm', 'a', 'i', 'n', '_', 'a', 'd', 'd', 0x00, 0x02,
        // Section 10: Code
        0x0a, 0x0f, 0x01, // Section size 15, 1 function body
        // func body 0 (main_add): (size 13 -> 11)
        0x0d, // body size 13
        0x00, // 0 locals
        0x41, 0x0a,       // i32.const 10
        0x41, 0x14,       // i32.const 20
        0x10, 0x00,       // call 0 (imported "add")
        0x41, 0x05,       // i32.const 5
        0x6a,             // i32.add
        0x10, 0x01,       // call 1 (imported "print_i32")
        0x0B
    };

    memset(&vm, 0, sizeof(vm));
    vm.code = wasm_module;
    vm.size = sizeof(wasm_module);

    // 読み込んだバイト列をダンプ
    dump_wasm_code(vm.code, vm.size);

    // 1. モジュールをパース
    parse_sections(&vm);

    // --- ADD: 実行開始前のプロローグ処理 ---
    ExportFunc *f_main = find_export(&vm, "main_add");
    if (f_main) {
        printf("Executing exported function 'main_add'...\n");
        vm.pc = vm.func_pcs[f_main->func_idx];

        // 関数のプロローグ: ローカル変数宣言をパース
        uint32_t local_groups = read_uLEB128(vm.code, &vm.pc);
        for (uint32_t i = 0; i < local_groups; i++) {
            (void)read_uLEB128(vm.code, &vm.pc); // num_locals
            (void)vm.code[vm.pc++]; // type
        }
    }
    // --- END ADD ---

    // 2. ホスト関数を登録
    vm_register_import(&vm, "env", "add", imported_add);
    vm_register_import(&vm, "env", "print_i32", print_i32);

    // 3. エクスポートされた関数を探して実行
    ExportFunc *f = find_export(&vm, "main_add");
    if (f) {
        // 実行開始PCはプロローグ処理で設定済み
        run(&vm);
        printf("Execution finished.\n");
    } else {
        printf("Export function 'main_add' not found.\n");
    }
    printf("--------------------\n");

    // --- テストケース8: ファイルからWasmモジュールを読み込んで実行 ---
    printf("--- Test Case 8: Full module parsing and execution from file ---\n");
    const char* wasm_file_path = "main.wasm";
    uint8_t *wasm_code = NULL;
    size_t wasm_size = 0;

    if (read_wasm_file(wasm_file_path, &wasm_code, &wasm_size) == 0) {
        memset(&vm, 0, sizeof(vm));
        vm.code = wasm_code;
        vm.size = wasm_size;

        // 読み込んだバイト列をダンプ
        dump_wasm_code(vm.code, vm.size);

        // 1. モジュールをパース
        parse_sections(&vm);

        // --- ADD: 実行開始前のプロローグ処理 ---
        ExportFunc *f_file_main = find_export(&vm, "main_add");
        if (f_file_main) {
            printf("Executing exported function 'main_add'...\n");
            vm.pc = vm.func_pcs[f_file_main->func_idx];

            // 関数のプロローグ: ローカル変数宣言をパース
            uint32_t local_groups = read_uLEB128(vm.code, &vm.pc);
            for (uint32_t i = 0; i < local_groups; i++) {
                (void)read_uLEB128(vm.code, &vm.pc); // num_locals
                (void)vm.code[vm.pc++]; // type
            }
        }
        // --- END ADD ---

        // 2. ホスト関数を登録
        vm_register_import(&vm, "env", "add", imported_add);
        vm_register_import(&vm, "env", "print_i32", print_i32);

        // 3. エクスポートされた関数を探して実行
        ExportFunc *f_file = find_export(&vm, "main_add");
        if (f_file) {
            // 実行開始PCはプロローグ処理で設定済み
            run(&vm);
            printf("Execution finished.\n");
        } else {
            printf("Export function 'main_add' not found.\n");
        }

        free(wasm_code); // 読み込んだメモリを解放
    }
    printf("--------------------\n");

    // --- テストケース9: ファイルからDataセクションを含むWasmモジュールを読み込んで実行 ---
    printf("--- Test Case 9: Full module with data section from file ---\n");
    const char* wasm_data_file_path = "data.wasm";
    uint8_t *wasm_data_code = NULL;
    size_t wasm_data_size = 0;

    if (read_wasm_file(wasm_data_file_path, &wasm_data_code, &wasm_data_size) == 0) {
        memset(&vm, 0, sizeof(vm));
        vm.code = wasm_data_code;
        vm.size = wasm_data_size;

        // 読み込んだバイト列をダンプ
        dump_wasm_code(vm.code, vm.size);

        // 1. モジュールをパース
        parse_sections(&vm);

        // --- ADD: 実行開始前のプロローグ処理 ---
        ExportFunc *f_data_main = find_export(&vm, "read_and_print");
        if (f_data_main) {
            printf("Executing exported function 'read_and_print'...\n");
            vm.pc = vm.func_pcs[f_data_main->func_idx];

            // 関数のプロローグ: ローカル変数宣言をパース
            uint32_t local_groups = read_uLEB128(vm.code, &vm.pc);
            for (uint32_t i = 0; i < local_groups; i++) {
                (void)read_uLEB128(vm.code, &vm.pc); // num_locals
                (void)vm.code[vm.pc++]; // type
            }
        }
        // --- END ADD ---

        // 2. ホスト関数を登録
        vm_register_import(&vm, "env", "print_i32", print_i32);

        // 3. エクスポートされた関数を探して実行
        ExportFunc *f_data = find_export(&vm, "read_and_print");
        if (f_data) {
            // 実行開始PCはプロローグ処理で設定済み
            run(&vm);
            printf("Execution finished.\n");
        } else {
            printf("Export function 'read_and_print' not found.\n");
        }
        free(wasm_data_code);
    }
    printf("--------------------\n");

    // --- テストケース10: WASIのfd_writeを含むWasmモジュールを実行 ---
    printf("--- Test Case 10: WASI fd_write from file ---\n");
    const char* wasm_wasi_file_path = "hello-wat.wasm";
    uint8_t *wasm_wasi_code = NULL;
    size_t wasm_wasi_size = 0;

    if (read_wasm_file(wasm_wasi_file_path, &wasm_wasi_code, &wasm_wasi_size) == 0) {
        memset(&vm, 0, sizeof(vm));
        vm.code = wasm_wasi_code;
        vm.size = wasm_wasi_size;

        // 読み込んだバイト列をダンプ
        dump_wasm_code(vm.code, vm.size);

        // 1. モジュールをパース
        parse_sections(&vm);

        // --- ADD: 実行開始前のプロローグ処理 ---
        ExportFunc *f_wasi_main = find_export(&vm, "_start");
        if (f_wasi_main) {
            printf("Executing exported function '_start'...\n");
            vm.pc = vm.func_pcs[f_wasi_main->func_idx];

            // 関数のプロローグ: ローカル変数宣言をパース
            uint32_t local_groups = read_uLEB128(vm.code, &vm.pc);
            for (uint32_t i = 0; i < local_groups; i++) {
                (void)read_uLEB128(vm.code, &vm.pc); // num_locals
                (void)vm.code[vm.pc++]; // type
            }
        }
        // --- END ADD ---

        // 2. ホスト関数を登録
        vm_register_import(&vm, "wasi_snapshot_preview1", "fd_write", wasi_fd_write);

        // 3. エクスポートされた関数を探して実行
        ExportFunc *f_wasi = find_export(&vm, "_start");
        if (f_wasi) {
            // 実行開始PCはプロローグ処理で設定済み
            run(&vm);
            printf("Execution finished.\n");
        } else {
            printf("Export function '_start' not found.\n");
        }
        free(wasm_wasi_code);
    }
    printf("--------------------\n");

    // --- テストケース11: 再帰呼び出しによるフィボナッチ数の計算 ---
    printf("--- Test Case 11: Recursive Fibonacci from file ---\n");
    uint8_t wasm_fib_module[] = {
        // Magic + Version
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // Section 1: Type
        0x01, 0x07, 0x01, // Section size 7, 1 type
        // type 0: (i32) -> i32
        0x60, 0x01, 0x7f, 0x01, 0x7f,
        // Section 3: Function
        0x03, 0x02, 0x01, 0x00, // Section size 2, 1 function, type_idx 0
        // Section 7: Export
        0x07, 0x07, 0x01, // Section size 7, 1 export
        // export "fib" -> func_idx 0
        0x03, 'f', 'i', 'b', 0x00, 0x00,
        // Section 10: Code
        0x0a, 0x1c, 0x01, // Section size 28, 1 function body
        // func body 0 (fib):
        0x1a, // body size 26
        0x00, // 0 locals
        // if (n <= 1) return n;
        0x20, 0x00,       // local.get 0
        0x41, 0x01,       // i32.const 1
        0x4d,             // i32.le_u (or 4c for le_s)
        0x04, 0x7f,       // if (result i32)
        0x20, 0x00,       //   local.get 0
        // else return fib(n-1) + fib(n-2);
        0x05,             // else
        0x20, 0x00,       //   local.get 0
        0x41, 0x01,       //   i32.const 1
        0x6b,             //   i32.sub
        0x10, 0x00,       //   call 0 (fib)
        0x20, 0x00,       //   local.get 0
        0x41, 0x02,       //   i32.const 2
        0x6b,             //   i32.sub
        0x10, 0x00,       //   call 0 (fib)
        0x6a,             //   i32.add
        0x0b,             // end
        0x0b              // end of function
    };

    memset(&vm, 0, sizeof(vm));
    vm.code = wasm_fib_module;
    vm.size = sizeof(wasm_fib_module);

    // 1. モジュールをパース
    parse_sections(&vm);

    // --- ADD: 実行開始前のプロローグ処理 ---
    ExportFunc *f_fib_main = find_export(&vm, "fib");
    if (f_fib_main) {
        printf("Executing exported function 'fib(5)'...\n");
        vm.pc = vm.func_pcs[f_fib_main->func_idx];
        vm.stack[vm.sp++] = 5; // 引数として 5 をスタックに積む

        // 関数のプロローグ: 引数をローカル変数へ
        FuncType *ftype = &vm.func_types[vm.func_type_indices[f_fib_main->func_idx]];
        for (int i = ftype->param_count - 1; i >= 0; i--) {
            vm.locals[i] = vm.stack[--vm.sp];
        }
        // 関数のプロローグ: ローカル変数宣言をパース
        uint32_t local_groups = read_uLEB128(vm.code, &vm.pc);
        for (uint32_t i = 0; i < local_groups; i++) {
            (void)read_uLEB128(vm.code, &vm.pc); // num_locals
            (void)vm.code[vm.pc++]; // type
        }
    }
    // --- END ADD ---

    // 2. エクスポートされた関数を探して実行
    ExportFunc *f_fib = find_export(&vm, "fib");
    if (f_fib) {
        run(&vm);
        printf("fib(5) = %d (expected 5)\n", vm.stack[vm.sp-1]);
    } else {
        printf("Export function 'fib' not found.\n");
    }
    printf("--------------------\n");

    return 0;

}
