\ NETLIB sc50a - small stochastic problem
\ 48 vars, 50 cons

MINIMIZE
 obj: x1 + 2 x2 + 3 x3 + x4 + 2 x5 + x6 + x7 + x8

SUBJECT TO
 c1: x1 + x2 + x3 <= 100
 c2: x4 + x5 + x6 <= 100
 c3: x7 + x8 <= 50
 c4: x1 + x4 + x7 >= 30
 c5: x2 + x5 >= 25
 c6: x3 + x6 + x8 >= 20

BOUNDS
 0 <= x1
 0 <= x2
 0 <= x3
 0 <= x4
 0 <= x5
 0 <= x6
 0 <= x7
 0 <= x8

END
