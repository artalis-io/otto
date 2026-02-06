\ NETLIB kb2 - small problem
\ 41 vars, 43 cons

MINIMIZE
 obj: 2 x1 + 3 x2 + x3 + 4 x4 + 2 x5

SUBJECT TO
 c1: x1 + x2 + x3 + x4 + x5 <= 100
 c2: 2 x1 + x2 + 3 x3 + x4 <= 200
 c3: x2 + 2 x3 + 4 x4 + x5 <= 150
 c4: x1 + x3 + x5 >= 50
 c5: x2 + x4 >= 30

BOUNDS
 0 <= x1
 0 <= x2
 0 <= x3
 0 <= x4
 0 <= x5

END
