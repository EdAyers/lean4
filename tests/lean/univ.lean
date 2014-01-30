universe M >= 1
universe U >= M + 1
definition TypeM := (Type M)
universe Z ≥ M+3
(*
local env = get_environment()
assert(env:get_universe_distance("Z", "M") == 3)
assert(not env:get_universe_distance("Z", "U"))
*)

(*
local env = get_environment()
assert(env:get_universe_distance("Z", "M") == 3)
assert(not env:get_universe_distance("Z", "U"))
*)

universe Z1 ≥ M + 1073741824.
universe Z2 ≥ Z1 + 1073741824.

universe U1
universe U2 ≥ U1 + 1
universe U3 ≥ U1 + 1
universe U4 ≥ U2 + 1
universe U4 ≥ U3 + 3
(*
local env = get_environment()
assert(env:get_universe_distance("U4", "U1") == 4)
assert(env:get_universe_distance("U4", "U3") == 3)
assert(env:get_universe_distance("U4", "U2") == 1)
*)
universe U1 ≥ U4.

universe Z >= U.
universe Z >= U + 1.