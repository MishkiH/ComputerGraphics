#include <vector>
#include <cmath>
#include <cstdlib>
#include <limits>
#include "tgaimage.h"
#include "matrix.h"
#include "model.h"
#include "camera.h"
#include "geometry.h"

Model *model = NULL;
const int width  = 1000;
const int height = 1000;
const int depth  = 255;

Matrix viewport(int x, int y, int w, int h) {
    Matrix m = Matrix::identity(4);
    m[0][3] = x + w / 2.f;
    m[1][3] = y + h / 2.f;
    m[2][3] = depth / 2.f;

    m[0][0] = w / 2.f;
    m[1][1] = h / 2.f;
    m[2][2] = depth / 2.f;

    return m;
}

Vec3f barycentric(Vec3f A, Vec3f B, Vec3f C, Vec3f P) {
    Vec3f s[2];
    for (int i=2; i--; ) {
        s[i][0] = C[i]-A[i];
        s[i][1] = B[i]-A[i];
        s[i][2] = A[i]-P[i];
    }

    Vec3f u = s[0] ^ s[1];
    
    if (std::abs(u[2])>1e-2) return Vec3f(1.f-(u.x+u.y)/u.z, u.y/u.z, u.x/u.z);
    return Vec3f(-1,1,1); // тчк вне треугольника
}

void line(Vec2i p0, Vec2i p1, TGAImage &image, TGAColor color) {
    bool steep = false;
    if (std::abs(p0.x-p1.x)<std::abs(p0.y-p1.y)) {
        std::swap(p0.x, p0.y);
        std::swap(p1.x, p1.y);
        steep = true;
    }
    if (p0.x>p1.x) {
        std::swap(p0.x, p1.x);
        std::swap(p0.y, p1.y);
    }

    for (int x=p0.x; x<=p1.x; x++) {
        float t = (x-p0.x)/(float)(p1.x-p0.x);
        int y = p0.y*(1.-t) + p1.y*t;
        if (steep) {
            image.set(y, x, color);
        } else image.set(x, y, color);
    }
}

void triangle(Vec3i *pts, Vec2f *uvs, float *zbuffer, TGAImage &image, float intensity) {
    Vec2i bboxmin(image.get_width()-1,  image.get_height()-1);
    Vec2i bboxmax(0, 0);
    Vec2i clamp(image.get_width()-1, image.get_height()-1);
    
    for (int i=0; i<3; i++) {
        for (int j=0; j<2; j++) {
            bboxmin.raw[j] = std::max(0, std::min(bboxmin.raw[j], pts[i].raw[j]));
            bboxmax.raw[j] = std::min(clamp.raw[j], std::max(bboxmax.raw[j], pts[i].raw[j]));
        }
    }
    
    Vec3f P;
    for (P.x=bboxmin.x; P.x<=bboxmax.x; P.x++) {
        for (P.y=bboxmin.y; P.y<=bboxmax.y; P.y++) {
            
            Vec3f bc_screen = barycentric(Vec3f(pts[0].x, pts[0].y, pts[0].z), 
                Vec3f(pts[1].x, pts[1].y, pts[1].z), Vec3f(pts[2].x, pts[2].y, pts[2].z), P);
            
            if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) continue;
            
            P.z = 0;
            P.z += pts[0].z * bc_screen.x;
            P.z += pts[1].z * bc_screen.y;
            P.z += pts[2].z * bc_screen.z;
            
            int idx = int(P.x) + int(P.y)*width;
            if (zbuffer[idx] < P.z) {
                zbuffer[idx] = P.z;
                
                Vec2f uv;
                uv.x = uvs[0].x * bc_screen.x + uvs[1].x * bc_screen.y + uvs[2].x * bc_screen.z;
                uv.y = uvs[0].y * bc_screen.x + uvs[1].y * bc_screen.y + uvs[2].y * bc_screen.z;
                
                TGAColor color = model->diffuse(uv);
                
                color.r *= intensity;
                color.g *= intensity;
                color.b *= intensity;
                
                image.set(P.x, P.y, color);
            }
        }
    }
}


int main() {
    model = new Model("obj/almost_african_head.obj");

    TGAImage image(width, height, TGAImage::RGB);
    
    float *zbuffer = new float[width*height];
    for (int i=0; i<width*height; i++) 
        zbuffer[i] = -std::numeric_limits<float>::max();

	Camera camera(
        Vec3f(0, 0, 1), // eye
        Vec3f(0, 0, 0), // center
        Vec3f(0, 1, 0) // up
    );

    Matrix View = camera.view();
    Matrix Projection = camera.projection();
    Matrix ViewPort = viewport(width/8, height/8, width*3/4, height*3/4);

    Vec3f light_dir(0,0,-1);

    for (int i=0; i<model->nfaces(); i++) {
        std::vector<int> face = model->face(i);
        std::vector<int> face_uv = model->face_uv(i);

        Vec3i screen_coords[3];
        Vec3f world_coords[3];
        Vec2f uv_coords[3];

        for (int j=0; j<3; j++) {
            Vec3f v = model->vert(face[j]);
            world_coords[j]  = v;
            
			Matrix v4 = embed(v); // (x,y,z,1)
			Matrix clip = ViewPort * Projection * View * v4;
			Vec3f screenf = project(clip); // перспективное деление
			screen_coords[j] = Vec3i(
				int(screenf.x + 0.5f),
				int(screenf.y + 0.5f),
				int(screenf.z + 0.5f)
			);

			uv_coords[j] = model->uv(face_uv[j]);
        }

        Vec3f n = (world_coords[2]-world_coords[0])^(world_coords[1]-world_coords[0]);
        n.normalize();
        float intensity = n*light_dir;

        if (intensity>0) {
            triangle(screen_coords, uv_coords, zbuffer, image, intensity);
        }
    }

    image.flip_vertically(); 
    image.write_tga_file("output.tga");

    delete [] zbuffer;
    delete model;
}