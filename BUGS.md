Known bugs and limitations
=========================

Bugs
----

Here is a list of know bugs, ordered by priority from high to low:

- ~~Viewport3DTab SIGSEGV on project load (crash in `onFrameChanged`): `_frameSpin`
  and several other Viewport3DTab pointer members were declared but never
  initialized (and `_frameSpin` is never created), so `onFrameChanged` dereferenced
  a garbage pointer when `TimeLine::frameChanged` fired during the load seek. Only
  surfaced once the `frameChanged -> onFrameChanged(SequenceTime,int)` connection
  was fixed to actually fire.~~ **FIXED 2026-06-14 (2c8fda570)** — NULL-init the
  Viewport3DTab pointer members + guard `onFrameChanged`.


Limitations
-----------


Missing and wanted features
---------------------------

Here is a list of non-blocking bugs / wanted features:

- implement Fields and Field Rendering:
  <http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#ImageEffectsFieldRendering>

- support Half float images
