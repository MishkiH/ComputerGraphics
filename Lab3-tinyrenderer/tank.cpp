#include <cmath>
#include <algorithm>
#include "tank.h"

const int STEPS = 60;
const float MAX_DIST = 15.0f;
const float SURF_DIST = 0.02f;

float sdBox(const Vec3f &p, const Vec3f &b) {
    Vec3f q(std::fabs(p.x), std::fabs(p.y), std::fabs(p.z));
    q = q-b;
    float dx = std::max(q.x, 0.0f);
    float dy = std::max(q.y, 0.0f);
    float dz = std::max(q.z, 0.0f);
    float outside = std::sqrt(dx*dx + dy*dy + dz*dz);
    float inside  = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);

    return outside+inside;
}

float mapTank(Vec3f p) {
    float x = -p.z;
    float y = p.y;
    float z = p.x;
    Vec3f pr(x, y, z);

    float d = sdBox(pr - Vec3f(0, 0.3, 0), Vec3f(0.7, 0.2, 0.5));
    float turret = sdBox(pr - Vec3f(0, 0.7, 0), Vec3f(0.4, 0.2, 0.3));
    float gun = sdBox(pr - Vec3f(0, 0.7, 0.7), Vec3f(0.05, 0.05, 0.5));
    d = std::min(std::min(d, turret), std::min(d, gun));

    return d;
}

Vec3f tankNormal(Vec3f p) {
    const float e = 0.01f;
    float dx = mapTank(Vec3f(p.x + e, p.y, p.z)) - mapTank(Vec3f(p.x-e, p.y, p.z));
    float dy = mapTank(Vec3f(p.x, p.y + e, p.z)) - mapTank(Vec3f(p.x, p.y-e, p.z));
    float dz = mapTank(Vec3f(p.x, p.y, p.z + e)) - mapTank(Vec3f(p.x, p.y, p.z-e));
    Vec3f n(dx, dy, dz);
    return n.normalize();
}

bool raymarchTank(const Vec3f &cameraPos, const Vec3f &rayDir, float &tHit) {
    float rayDistance = 0;
    for (int i = 0; i < STEPS; i++) {
        Vec3f p = cameraPos + rayDir*rayDistance;
        float d = mapTank(p);
        if (d < SURF_DIST) {
            tHit = rayDistance;
            return true;
        }
        rayDistance += d;
        if (rayDistance > MAX_DIST) break;
    }
    return false; //рандеву танчика и луча не состоялся(ось) хз
}

void render_tank(TGAImage &image) {
    const int tankSize = 600;

    int imgW = image.get_width();
    int imgH = image.get_height();

    Vec3f cameraPos(1, 1, -5);

    for (int y = 0; y < tankSize; y++) {
        for (int x = 0; x < tankSize; x++) {
            int ix = x; // где х
            int iy = y + 400; // где у
            if (ix < 0 || iy < 0 || ix >= imgW || iy >= imgH) continue;

            float u = (x/float(tankSize))*2 - 1;
            float v = (y/float(tankSize))*2 - 1; //[-1; 1]

            Vec3f rayDir(u, v, 1);
            rayDir.normalize();

            float tHit;
            if (raymarchTank(cameraPos, rayDir, tHit)) {
                Vec3f p = cameraPos + rayDir*tHit;
                Vec3f norm = tankNormal(p);
                float diff = 0.5f * (norm.y + 1.0f);

                image.set(ix, iy, TGAColor(0, diff*200, 0, 255));
}}}}