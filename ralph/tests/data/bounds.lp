\ Test all bound types

MINIMIZE
 obj: x1 + x2 + x3 + x4 + x5 + x6

SUBJECT TO
 c1: x1 + x2 + x3 + x4 + x5 + x6 >= 10

BOUNDS
 0 <= x1 <= 100
 x2 >= 5
 x3 <= 50
 x4 = 10
 x5 free
 -infinity <= x6 <= 25

END
