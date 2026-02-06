\ NETLIB sc50b - small stochastic problem variant
\ 48 vars, 50 cons

MINIMIZE
 obj: 2 x1 + x2 + 3 x3 + 2 x4 + x5 + x6 + 2 x7 + x8

SUBJECT TO
 c1: x1 + x2 + x3 <= 120
 c2: x4 + x5 + x6 <= 110
 c3: x7 + x8 <= 60
 c4: x1 + x4 + x7 >= 35
 c5: x2 + x5 >= 30
 c6: x3 + x6 + x8 >= 25

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
