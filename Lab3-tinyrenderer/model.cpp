#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include "model.h"

Model::Model(const char *filename) : verts_(), uv_(), faces_(), faces_uv_(), diffusemap_() {
    std::ifstream in;
    in.open (filename, std::ifstream::in);
    if (in.fail()) return;
    std::string line;
    while (!in.eof()) {
        std::getline(in, line);
        std::istringstream iss(line.c_str());
        char trash;
        if (!line.compare(0, 2, "v ")) {
            iss >> trash;
            Vec3f v;
            for (int i=0;i<3;i++) iss >> v.raw[i];
            verts_.push_back(v);
        } else if (!line.compare(0, 3, "vt ")) {
            iss >> trash >> trash;
            Vec2f uv;
            for (int i=0;i<2;i++) iss >> uv.raw[i];
            uv_.push_back(uv);
        } else if (!line.compare(0, 2, "f ")) {
            std::vector<int> f;
            std::vector<int> f_uv;
            int v_idx, vt_idx, vn_idx;
            iss >> trash;
            while (iss >> v_idx >> trash >> vt_idx >> trash >> vn_idx) {
                f.push_back(v_idx-1);
                f_uv.push_back(vt_idx-1);
            }
            faces_.push_back(f);
            faces_uv_.push_back(f_uv);
        }
    }
    std::cerr << "# v# " << verts_.size() << " f# "  << faces_.size() << " vt# " << uv_.size() << std::endl;
    load_texture(filename, "_diffuse.tga", diffusemap_);
}

Model::~Model() {
}

int Model::nverts() {
    return (int)verts_.size();
}

int Model::nfaces() {
    return (int)faces_.size();
}

std::vector<int> Model::face(int idx) {
    return faces_[idx];
}

std::vector<int> Model::face_uv(int idx) {
    return faces_uv_[idx];
}

Vec3f Model::vert(int i) {
    return verts_[i];
}

Vec2f Model::uv(int i) {
    return uv_[i];
}

void Model::load_texture(std::string filename, const char *suffix, TGAImage &img) {
    std::string texfile(filename);
    size_t dot = texfile.find_last_of(".");
    if (dot!=std::string::npos) {
        texfile = texfile.substr(0,dot) + std::string(suffix);
        std::cerr << "Loading texture: " << texfile << "\n";
        img.read_tga_file(texfile.c_str());
        img.flip_vertically();
    }
}

TGAColor Model::diffuse(Vec2f uvf) {
    Vec2i uv(uvf.x * diffusemap_.get_width(), uvf.y * diffusemap_.get_height());
    return diffusemap_.get(uv.x, uv.y);
}