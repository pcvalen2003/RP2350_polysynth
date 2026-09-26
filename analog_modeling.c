//Aproximación de tanh (rango de entrada esperado +/- 32768)
static inline int32_t soft_clip(int32_t x) {
    const int32_t limit = 32767;
    
    // Hard clipping de seguridad en los extremos
    if (x >= limit) return 21845;  // Aproximadamente 2/3 del máximo
    if (x <= -limit) return -21845;

    // x^3 / (32768^2) usando hardware mults de 64 bits para evitar overflow
    int64_t x2 = ((int64_t)x * x) >> 15;
    int32_t x3 = (int32_t)(((int64_t)x2 * x) >> 15);

    // Ecuación final: x - x^3/3
    return x - (x3 / 3);
}

