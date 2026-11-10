//
// cl_csp.h - Client-Side Prediction (Ghost) for high-latency Quake2 play
//
// Predicts other players' future positions and renders translucent "ghost"
// entities alongside (or instead of) the server's interpolated entities.
// Target: visual prediction for 140-175ms ping to overseas servers.
//

#ifndef CL_CSP_H
#define CL_CSP_H

//
// csp_enable values
//
#define CSP_OFF          0  // Ghost prediction disabled
#define CSP_GHOST_BOTH   1  // Render real entity + translucent ghost
#define CSP_GHOST_ONLY   2  // Render only the predicted ghost entity

//
// Public API
//
void    CL_CSP_ValueChanged (struct cvar_s *self, char *oldValue, char *newValue);          // Called when csp_enable changes
void    CL_CSP_ParseFrameNotify (void);                                                     // Called at top of CL_ParseFrame
void    CL_CSP_UpdateRTT (float rtt_ms);                                                    // Feed RTT sample from cl_scrn.c
void    CL_CSP_PredictEntity (centity_t *cent, int ent_num, vec3_t out_origin, vec3_t out_angles);
qboolean CL_CSP_ShouldRenderGhost (centity_t *cent, int ent_num);

#endif
