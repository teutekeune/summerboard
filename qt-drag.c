/* Qt cancels a button click when the finger moves more than
   startDragDistance (10px). Toolbox icons are about 24px, so a
   normal tap never selects the tool. */
int _ZNK11QStyleHints17startDragDistanceEv(void *self) {
  (void)self;
  return 36;
}
