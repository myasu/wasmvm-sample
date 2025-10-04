(module
  (import "env" "print_i32" (func $print_i32 (param i32)))
  (memory 1)
  (data (i32.const 10) "\0a\00\00\00") ;; メモリアドレス10に 10 を書き込む
  (func (export "read_and_print")
    i32.const 10      ;; 読み込むメモリアドレス
    i32.load          ;; メモリからi32をロード
    call $print_i32   ;; ロードした値をprint_i32で表示
  )
)
