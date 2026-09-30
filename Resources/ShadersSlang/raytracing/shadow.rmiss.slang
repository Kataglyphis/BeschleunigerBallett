// A shadow ray that misses everything means the point is lit.

[shader("miss")]
void shadow_rmiss_main(inout bool shadowed)
{
    shadowed = false;
}