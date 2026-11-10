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
void    CL_CSP_SmoothChanged (struct cvar_s *self, char *oldValue, char *newValue);         // Called when csp_smooth changes
void    CL_CSP_ParseFrameNotify (void);                                                     // Called at top of CL_ParseFrame (timestamp only)
void    CL_CSP_FrameValidated (void);                                                       // Phase E4: called after validated frame + entity parse
void    CL_CSP_UpdateRTT (float rtt_ms);                                                    // Feed RTT sample (internal EMA update)
void    CL_CSP_SampleRTT (void);                                                             // Decoupled RTT sampling (Req 3)
void    CL_CSP_ProcessSnapshot (void);                                                       // Phase E2: snapshot membership lifecycle
void    CL_CSP_PredictEntity (centity_t *cent, int ent_num, vec3_t out_origin, vec3_t out_angles);
qboolean CL_CSP_ShouldRenderGhost (centity_t *cent, int ent_num);
void    CL_CSP_LogRender (int ent_num, vec3_t engine_interpolated_origin, vec3_t smoothed_ghost_origin, qboolean original_entity_rendered, int ghost_mode);

#endif
