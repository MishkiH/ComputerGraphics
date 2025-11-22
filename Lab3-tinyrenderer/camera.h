// camera.h
#ifndef __CAMERA_H__
#define __CAMERA_H__

#include "geometry.h"
#include "matrix.h"

class Camera {
public:
    Vec3f eye; // где стоит камера
    Vec3f center; // куда смотрит
    Vec3f up; // что считаем верхом

    Camera(const Vec3f& e = Vec3f(1,1,3),
           const Vec3f& c = Vec3f(0,0,0),
           const Vec3f& u = Vec3f(0,1,0))
        : eye(e), center(c), up(u) {}

    Matrix view() const {
        // базис камеры: z - назад, x - вправо, y - вверх
        Vec3f z = (eye - center).normalize();
        Vec3f x = (up ^ z).normalize();
        Vec3f y = (z ^ x).normalize();

        Matrix m = Matrix::identity(4);
        for (int i = 0; i < 3; ++i) {
            m[0][i] = x[i];
            m[1][i] = y[i];
            m[2][i] = z[i];
        }

        Matrix t = Matrix::identity(4);
        t[0][3] = -eye.x;
        t[1][3] = -eye.y;
        t[2][3] = -eye.z;

        return m * t;
    }

    float zoom = 1.0f;
    float focus = 4.0f;

    Matrix projection() const {
        Matrix p = Matrix::identity(4);
        float f = focus / zoom;
        p[3][2] = -1.f / f;
        return p;
    }

    void changeZoom(float factor) {
    zoom *= factor;
}
};

#endif // __CAMERA_H__
