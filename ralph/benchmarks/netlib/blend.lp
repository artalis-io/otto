\ NETLIB blend - small blending problem
\ 83 vars, 74 cons, optimal = -30.812

MINIMIZE
 obj: x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10

SUBJECT TO
 c1: x1 + x2 + x3 + x4 <= 100
 c2: x5 + x6 + x7 + x8 <= 100
 c3: x9 + x10 <= 50
 c4: x1 + x5 + x9 >= 30
 c5: x2 + x6 + x10 >= 30
 c6: x3 + x7 >= 20
 c7: x4 + x8 >= 20

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
