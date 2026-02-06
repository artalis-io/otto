\ MIP test problem with GENERAL and BINARY variables

MAXIMIZE
 profit: 10 x1 + 6 x2 + 4 x3

SUBJECT TO
 c1: x1 + x2 + x3 <= 100
 c2: 10 x1 + 4 x2 + 5 x3 <= 600
 c3: 2 x1 + 2 x2 + 6 x3 <= 300

BOUNDS
 0 <= x1 <= 40
 0 <= x2 <= 50
 0 <= x3 <= 30
 y1 free

GENERAL
 x1 x2 x3

BINARY
 y1

END
