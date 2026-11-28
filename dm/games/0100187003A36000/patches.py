"""Use a one-tick frame period for Let's Go, Eevee!"""
from engine_patches import lets_go_frame_period_binding, lets_go_frame_periods

MATCH_CUTS = {'main': [lets_go_frame_periods]}
BINDING = lets_go_frame_period_binding
