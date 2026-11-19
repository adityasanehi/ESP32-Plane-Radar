#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Call every loop: smoothly shrinks the info card's countdown ring between
 *  full redraws. Cheap no-op when no card is showing. */
void radarDisplayAnimate();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

}  // namespace ui
