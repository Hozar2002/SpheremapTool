#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <cassert>
#include <vector>
#include <iterator>

#include "stb_image.hpp"
#include "stb_image_write.hpp"

typedef uint8_t u8;
typedef uint32_t u32;

struct Image {
	int width, height;
	std::unique_ptr<u8, std::function<void(u8*)>> data;

	Image() :
		width(-1), height(-1)
	{}

	Image(const std::string& filename) :
		data(stbi_load(filename.c_str(), &width, &height, nullptr, 4), stbi_image_free)
	{}

	Image& operator= (Image&& o) {
		width = o.width;
		height = o.height;
		data.swap(o.data);

		return *this;
	}

private:
	Image& operator= (const Image&);
};

struct Cubemap {
	enum CubeFace {
		FACE_POS_X, FACE_NEG_X,
		FACE_POS_Y, FACE_NEG_Y,
		FACE_POS_Z, FACE_NEG_Z,
		NUM_FACES
	};

	Image faces[NUM_FACES];

	Cubemap(const std::string& fname_prefix, const std::string& fname_extension) {
		faces[FACE_POS_X] = Image(fname_prefix + "_right."  + fname_extension);
		faces[FACE_NEG_X] = Image(fname_prefix + "_left."   + fname_extension);
		faces[FACE_POS_Y] = Image(fname_prefix + "_top."    + fname_extension);
		faces[FACE_NEG_Y] = Image(fname_prefix + "_bottom." + fname_extension);
		faces[FACE_POS_Z] = Image(fname_prefix + "_front."  + fname_extension);
		faces[FACE_NEG_Z] = Image(fname_prefix + "_back."   + fname_extension);
	}

	u32 readTexel(CubeFace face, int x, int y) {
		assert(face < NUM_FACES);
		const Image& face_img = faces[face];

		assert(x < face_img.width);
		assert(y < face_img.height);

		const u32* img_data = reinterpret_cast<const u32*>(face_img.data.get());
		return img_data[y * face_img.width + x];
	}

	void computeTexCoords(float x, float y, float z, CubeFace& out_face, float& out_s, float& out_t) {
		int major_axis;

		float v[3] = { x, y, z };
		float a[3] = { std::abs(x), std::abs(y), std::abs(z) };

		if (a[0] >= a[1] && a[0] >= a[2]) {
			major_axis = 0;
		} else if (a[1] >= a[0] && a[1] >= a[2]) {
			major_axis = 1;
		} else if (a[2] >= a[0] && a[2] >= a[1]) {
			major_axis = 2;
		}

		if (v[major_axis] < 0.0f)
			major_axis = major_axis * 2 + 1;
		else
			major_axis *= 2;

		float tmp_s, tmp_t, m;
		switch (major_axis) {
			/* +X */ case 0: tmp_s = -z; tmp_t = -y; m = a[0]; break;
			/* -X */ case 1: tmp_s =  z; tmp_t = -y; m = a[0]; break;
			/* +Y */ case 2: tmp_s =  x; tmp_t =  z; m = a[1]; break;
			/* -Y */ case 3: tmp_s =  x; tmp_t = -z; m = a[1]; break;
			/* +Z */ case 4: tmp_s =  x; tmp_t = -y; m = a[2]; break;
			/* -Z */ case 5: tmp_s = -x; tmp_t = -y; m = a[2]; break;
		}

		out_face = CubeFace(major_axis);
		out_s = 0.5f * (tmp_s / m + 1.0f);
		out_t = 0.5f * (tmp_t / m + 1.0f);
	}

	u32 sampleFace(CubeFace face, float s, float t) {
		const Image& face_img = faces[face];

		// Point sampling for now
		int x = std::min(static_cast<int>(s * face_img.width ), face_img.width  - 1);
		int y = std::min(static_cast<int>(t * face_img.height), face_img.height - 1);
		return readTexel(face, x, y);
	}
};

inline float unlerp(int val, int max) {
	return (val + 0.5f) / max;
}

inline u32 makeColor(u8 r, u8 g, u8 b) {
	return r | g << 8 | b << 16 | 0xFF << 24;
}

inline void splitColor(u32 col, u8& r, u8& g, u8& b) {
	r = col >> 0  & 0xFF;
	g = col >> 8  & 0xFF;
	b = col >> 16 & 0xFF;
}

template <typename T>
inline typename T::value_type pop_from(T& container) {
	T::value_type val = container.back();
	container.pop_back();
	return val;
}

int main(int argc, char* argv[]) {
	if (argc < 1)
		return 1;

	int output_size = 1024;
	std::vector<std::string> positional_params;

	{
		std::vector<std::string> input_params(std::reverse_iterator<char**>(argv+argc), std::reverse_iterator<char**>(argv+1));

		while (!input_params.empty()) {
			std::string opt = pop_from(input_params);

			if (opt == "-") {
				std::copy(input_params.rbegin(), input_params.rend(), std::back_inserter(positional_params));
				break;
			} else if (opt == "-size") {
				output_size = std::stoi(pop_from(input_params));
			} else {
				positional_params.push_back(opt);
			}
		}
	}

	if (positional_params.size() != 2)
		return 1;

	std::string fname_prefix = positional_params[0];
	std::string fname_extension = positional_params[1];
	std::string output_fname = fname_prefix + "_spheremap.bmp";

	std::vector<u32> out_data(output_size * output_size);
	float output_pixel_size = 1.f / output_size;

	{
		Cubemap input_cubemap(fname_prefix, fname_extension);

		for (int y = 0; y < output_size; ++y) {
			for (int x = 0; x < output_size; ++x) {
				static const int NUM_SAMPLES = 5;
				static const float sample_offsets[NUM_SAMPLES*2] = {
					0.0f   , 0.0f   ,
					-.1875f, -.375f ,
					0.375f , -.1875f,
					0.1875f, 0.375f ,
					-.375f , 0.1875f,
				};

				float center_s = unlerp(x, output_size);
				float center_t = unlerp(y, output_size);

				u32 sample_r = 0, sample_g = 0, sample_b = 0;

				for (int sample = 0; sample < NUM_SAMPLES; ++sample) {
					float s = center_s + sample_offsets[sample*2 + 0] * output_pixel_size;
					float t = center_t + sample_offsets[sample*2 + 1] * output_pixel_size;

					float vx, vy, vz;
					float rev_p = 16.0f * (s - s*s + t - t*t) - 4.0f;
					if (rev_p < 0.0f) {
						vx = 0.0f;
						vy = 0.0f;
						vz = -1.0f;
					} else {
						float rev_p_sqrt = std::sqrtf(rev_p);
						vx = rev_p_sqrt * (2.0f * s - 1.0f);
						vy = rev_p_sqrt * -(2.0f * t - 1.0f);
						vz = 8.0f * (s - s*s + t - t*t) - 3.0f;
					}

					Cubemap::CubeFace cube_face;
					float tex_s, tex_t;
					input_cubemap.computeTexCoords(vx, vy, vz, cube_face, tex_s, tex_t);
					u32 sample_color = input_cubemap.sampleFace(cube_face, tex_s, tex_t);

					u8 r, g, b;
					splitColor(sample_color, r, g, b);
					sample_r += r;
					sample_g += g;
					sample_b += b;
				}

				sample_r /= NUM_SAMPLES;
				sample_g /= NUM_SAMPLES;
				sample_b /= NUM_SAMPLES;

				out_data[y * output_size + x] = makeColor(sample_r, sample_g, sample_b);
			}
		}
	}

	stbi_write_bmp(output_fname.c_str(), output_size, output_size, 4, out_data.data());
}