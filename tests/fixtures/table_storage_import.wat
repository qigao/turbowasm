(module
  (import "p" "wide" (table $wide i64 8 16 funcref))
  (func (param i64 i64 i64)
    local.get 0 local.get 1 local.get 2 table.copy $wide $wide)
  (func (result i64) table.size $wide)
)
