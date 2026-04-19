// =============================================================================
// hello_cardboard_app.h  — additions for sprite-sheet pose control
//
// Add these declarations to your existing hello_cardboard_app.h file.
// =============================================================================

// ---------------------------------------------------------------------------
// 1.  New public method — add inside the HelloCardboardApp class declaration.
// ---------------------------------------------------------------------------

  /**
   * Selects which cell of the sprite sheet is shown on the billboard quad.
   *
   * @param pose_index  0-based index into the 4-column × 3-row grid.
   *                    Range: [0, 11].  Out-of-range values are ignored.
   *
   * Thread-safety: safe to call from the Java/JNI thread; the resulting UV
   * change is consumed on the next GL draw call inside DrawTarget().
   */
  void SetRobotPose(int pose_index);

// ---------------------------------------------------------------------------
// 2.  New private member variables — add inside the private section.
// ---------------------------------------------------------------------------

  // Sprite-sheet uniform locations (resolved in OnSurfaceCreated).
  GLint sprite_offset_param_;  // location of u_SpriteOffset vec2
  GLint sprite_scale_param_;   // location of u_SpriteScale  vec2

  // Current sprite-cell UV origin (top-left corner of the active cell).
  // Updated atomically by SetRobotPose(); read every frame in DrawTarget().
  // Both values start at 0.0f (pose 0 = Welcome, col=0, row=0).
  float sprite_offset_u_;
  float sprite_offset_v_;
