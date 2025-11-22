#ifndef __MODEL_H__
#define __MODEL_H__

#include <vector>
#include "geometry.h"
#include "tgaimage.h"

class Model {
private:
    std::vector<Vec3f> verts_;
    std::vector<Vec2f> uv_; // текстурные координаты
    
    std::vector<std::vector<int> > faces_; // индексы вершин v
    std::vector<std::vector<int> > faces_uv_; // Индексы текстур vt (параллельно faces_)
    
    TGAImage diffusemap_; //картинка текстуры

    void load_texture(std::string filename, const char *suffix, TGAImage &img);

public:
    Model(const char *filename);
    ~Model();
    int nverts();
    int nfaces();
    Vec3f vert(int i);
    Vec2f uv(int i); //получить UV по индексу
    TGAColor diffuse(Vec2f uv); // получить цвет пикселя по UV координате
    std::vector<int> face(int idx);
    std::vector<int> face_uv(int idx); // получить индексы UV для грани
};

#endif //__MODEL_H__
