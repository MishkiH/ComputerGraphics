// matrix.h
#ifndef __MATRIX_H__
#define __MATRIX_H__

#include <vector>
#include <cassert>
#include "geometry.h"

class Matrix {
public:
    int rows, cols;
    std::vector<std::vector<float>> data;

    Matrix(int r=4, int c=4) : rows(r), cols(c), data(r, std::vector<float>(c, 0.f)) {}

    static Matrix identity(int n) {
        Matrix m(n, n);
        for (int i = 0; i < n; ++i) m[i][i] = 1.f;
        return m;
    }

    std::vector<float>& operator[](int r) {
        assert(r >= 0 && r < rows);
        return data[r];
    }

    const std::vector<float>& operator[](int r) const {
        assert(r >= 0 && r < rows);
        return data[r];
    }
};

inline Matrix operator*(const Matrix& a, const Matrix& b) {
    assert(a.cols == b.rows);
    Matrix result(a.rows, b.cols);
    for (int i = 0; i < a.rows; ++i) {
        for (int j = 0; j < b.cols; ++j) {
            float sum = 0.f;
            for (int k = 0; k < a.cols; ++k) {
                sum += a.data[i][k] * b.data[k][j];
            }
            result[i][j] = sum;
        }
    }
    return result;
}

// "запаковать" Vec3f в однородные координаты (x,y,z,1)^T
inline Matrix embed(const Vec3f& v) {
    Matrix m(4, 1);
    m[0][0] = v.x;
    m[1][0] = v.y;
    m[2][0] = v.z;
    m[3][0] = 1.f;
    return m;
}

// "распаковать" обратно: делим на w (перспектива)
inline Vec3f project(const Matrix& m) {
    float w = m[3][0];
    return Vec3f(m[0][0] / w, m[1][0] / w, m[2][0] / w);
}

#endif // __MATRIX_H__
