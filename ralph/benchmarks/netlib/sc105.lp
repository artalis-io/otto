\ NETLIB sc105 - medium stochastic problem
\ 103 vars, 105 cons

MINIMIZE
 obj: x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10

SUBJECT TO
 c1: x1 + x2 + x3 <= 100
 c2: x4 + x5 + x6 <= 100
 c3: x7 + x8 + x9 <= 100
 c4: x10 <= 50
 c5: x1 + x4 + x7 + x10 >= 80
 c6: x2 + x5 + x8 >= 60
 c7: x3 + x6 + x9 >= 40

BOUNDS
 0 <= x1
 0 <= x2
 0 <= x3
 0 <= x4
 0 <= x5
 0 <= x6
 0 <= x7
 0 <= x8
 0 <= x9
 0 <= x10

END
