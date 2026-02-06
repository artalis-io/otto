\ Simple LP test problem
\ Maximize 5x1 + 4x2 + 3x3
\ Subject to:
\   2x1 + 3x2 + x3 <= 100
\   4x1 + 2x2 + 3x3 <= 120
\ All variables >= 0

MAXIMIZE
 obj: 5 x1 + 4 x2 + 3 x3

SUBJECT TO
 labor: 2 x1 + 3 x2 + x3 <= 100
 machine: 4 x1 + 2 x2 + 3 x3 <= 120

END
