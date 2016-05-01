# PropRes_Receiver
PropResponse on the receiver side for WP messages on analog output objects.
In this implementation version, the number of challenge requests follows linear relationship between current value and target object values. This is changed in ao.c under demo/objects/ folder.

To swich to quadratic mode: go into ao.c: result = (int)pow(diff, 2)
