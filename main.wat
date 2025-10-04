(module
  ;; --- 1. 型定義セクション ---
  ;; 型0: (i32, i32) -> i32
  (type $t_add (func (param i32 i32) (result i32)))
  ;; 型1: (i32) -> ()
  (type $t_print (func (param i32)))
  ;; 型2: () -> ()
  (type $t_main (func))

  ;; --- 2. インポートセクション ---
  ;; "env"モジュールから2つの関数をインポートする
  (import "env" "add" (func $imported_add (type $t_add)))
  (import "env" "print_i32" (func $imported_print (type $t_print)))

  ;; --- 3. 関数セクション ---
  ;; モジュール内部の関数がどの型シグネチャを持つかを定義
  (func (type $t_main)) ;; $mainという名前を削除し、型インデックスのみを宣言

  ;; --- 4. エクスポートセクション ---
  ;; "main_add" という名前で内部関数をエクスポートする
  ;; インポートされた関数が2つあるため、内部関数のインデックスは2になる -> 3 では？
  (export "main_add" (func 3))

  ;; --- 5. コードセクション ---
  ;; 関数の実装 (名前なし)
  (func (type $t_main)
    i32.const 10
    i32.const 20
    call $imported_add   ;; add(10, 20) -> スタックに 30 が積まれる
    i32.const 5
    i32.add              ;; 30 + 5 -> スタックに 35 が積まれる
    call $imported_print ;; print_i32(35)
  )
)
