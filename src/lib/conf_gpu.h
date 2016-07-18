/*

 GPU optimized versions for conf and change.

 */

#ifndef VINA_CONF_GPU_H
#define VINA_CONF_GPU_H

#include "conf.h"
#include "matrix.h"
#include "gpu_util.h"

#define GNINA_CUDA_NUM_THREADS (512)
#define CUDA_KERNEL_LOOP(i, n) \
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; \
       i < (n); \
       i += blockDim.x * gridDim.x)

typedef triangular_matrix<fl> flmat;

inline __global__ void scalar_mult_kernel(float mult, const int n,
		float *vals) {
	CUDA_KERNEL_LOOP(index, n)
	{
		vals[index] *= mult;
	}
}

/* change is a single GPU allocated array of floats.
 * The first six are the position and orientation and the remaining
 * are torsions.  The class itself is allocated on the CPU (at least for now)
 * and GPU code must be passed raw vector.
 */
struct change_gpu {
	float *change_values;
	int n; //on cpu, size of change_values is 6+torsions

	change_gpu(const change& src) :
			change_values(NULL), n(0) {
		std::vector<float> data;
		//figure out number of torsions
		assert(src.ligands.size() == 1);
		n = 6; //position + orientation
		const ligand_change& lig = src.ligands[0];

		for (unsigned i = 0; i < 3; i++)
			data.push_back(lig.rigid.position[i]);

		for (unsigned i = 0; i < 3; i++)
			data.push_back(lig.rigid.orientation[i]);

		n += lig.torsions.size();
		for (unsigned i = 0, nn = lig.torsions.size(); i < nn; i++) {
			data.push_back(lig.torsions[i]);
		}

		for (unsigned i = 0, nn = src.flex.size(); i < nn; i++) {
			n += src.flex[i].torsions.size();
			for (unsigned j = 0, m = src.flex[i].torsions.size(); j < m; j++) {
				data.push_back(src.flex[i].torsions[j]);
			}
		}
		//allocate vector
		CUDA_CHECK(cudaMalloc(&change_values, sizeof(float) * n));
		//and init
		assert(n == data.size());
		CUDA_CHECK(
				cudaMemcpy(change_values, &data[0], n * sizeof(float),
						cudaMemcpyHostToDevice));
	}

	//allocate and copy
	change_gpu(const change_gpu& src) :
			n(0), change_values(NULL) {
		*this = src;
	}

	change_gpu& operator=(const change_gpu& src) {
		if (change_values == NULL || n < src.n) {
			if (change_values) {
				CUDA_CHECK(cudaFree(change_values));
			}
			CUDA_CHECK(cudaMalloc(&change_values, sizeof(float) * src.n));
		}
		n = src.n;
		CUDA_CHECK(
				cudaMemcpy(change_values, src.change_values, sizeof(float) * n,
						cudaMemcpyDeviceToDevice));
	}

	~change_gpu() {
		//deallocate mem
		CUDA_CHECK(cudaFree(change_values));
	}

	//dkoes - zeros out all differences
	void clear() {
		CUDA_CHECK(cudaMemset(change_values, 0, sizeof(float) * n));
	}

	//dkoes - multiply by -1
	void invert() {
		scalar_mult_kernel<<<1, min(GNINA_CUDA_NUM_THREADS, n)>>>(-1.0, n,
				change_values);
	}

	//return dot product
	float dot(const change_gpu& rhs) const {
		//since N is small, I think we should do a single warp of threads for this

		std::vector<float> a, b;
		get_data(a);
		rhs.get_data(b);
		fl tmp = 0;
		VINA_FOR(i, n)
			tmp += a[i] * b[i];
		return tmp;
	}

	//subtract rhs from this
	void sub(const change_gpu& rhs) {
		std::vector<float> a, b;
		get_data(a);
		rhs.get_data(b);
		VINA_FOR(i, n)
			a[i] -= b[i];
		set_data(a);
	}

	void minus_mat_vec_product(const flmat& m, change_gpu& out) const {
		std::vector<float> a;
		std::vector<float> b(n, 0);
		get_data(a);
		VINA_FOR(i, n) {
			fl sum = 0;
			VINA_FOR(j, n)
				sum += m(m.index_permissive(i, j)) * a[j];
			b[i] = -sum;
		}
		out.set_data(b);
	}

	sz num_floats() const {
		return n;
	}

	static bool bfgs_update(flmat& h, const change_gpu& p, const change_gpu& y,
			const fl alpha) {
		//perform bfgs update, eventually h will be gpu allocated
		std::vector<float> pvec;
		p.get_data(pvec);

		const fl yp = y.dot(p);
		if (alpha * yp < epsilon_fl)
			return false; // FIXME?

		change_gpu minus_hy(y);
		y.minus_mat_vec_product(h, minus_hy);

		const fl yhy = -y.dot(minus_hy);
		const fl r = 1 / (alpha * yp); // 1 / (s^T * y) , where s = alpha * p // FIXME   ... < epsilon
		const sz n = p.num_floats();

		std::vector<float> minus_hyvec;
		minus_hy.get_data(minus_hyvec);

		VINA_FOR(i, n)
			VINA_RANGE(j, i, n) // includes i
				h(i, j) += alpha * r
						* (minus_hyvec[i] * pvec[j] + minus_hyvec[j] * pvec[i])
						+ +alpha * alpha * (r * r * yhy + r) * pvec[i]
								* pvec[j]; // s * s == alpha * alpha * p * p	}
		return true;
	}

	//for debugging
	void get_data(std::vector<float>& d) const {
		d.resize(n);
		CUDA_CHECK(
				cudaMemcpy(&d[0], change_values, n * sizeof(float),
						cudaMemcpyDeviceToHost));

	}

	void set_data(std::vector<float>& d) const {
		CUDA_CHECK(
				cudaMemcpy(change_values, &d[0], n * sizeof(float),
						cudaMemcpyHostToDevice));
	}

	void print() const {
		std::vector<float> d;
		get_data(d);
		for (unsigned i = 0, n = d.size(); i < n; i++) {
			std::cout << d[i] << " ";
		}
		std::cout << "\n";
	}
};

union conf_info {
	struct {
		float position[3];
		float orientation[4];
		float torsions[];
	};
	float values[];
};

struct conf_gpu {

	conf_info *cinfo;
	int n; //on cpu, size of conf_values is 7+torsions to include x,y,z and quaternion

	conf_gpu(const conf& src) :
			cinfo(NULL), n(0) {
		std::vector<float> data;
		//figure out number of torsions
		assert(src.ligands.size() == 1);
		n = 7; //position + orientation(qt)
		const ligand_conf& lig = src.ligands[0];

		for (unsigned i = 0; i < 3; i++)
			data.push_back(lig.rigid.position[i]);

		data.push_back(lig.rigid.orientation.R_component_1());
		data.push_back(lig.rigid.orientation.R_component_2());
		data.push_back(lig.rigid.orientation.R_component_3());
		data.push_back(lig.rigid.orientation.R_component_4());

		n += lig.torsions.size();
		for (unsigned i = 0, nn = lig.torsions.size(); i < nn; i++) {
			data.push_back(lig.torsions[i]);
		}

		for (unsigned i = 0, nn = src.flex.size(); i < nn; i++) {
			n += src.flex[i].torsions.size();
			for (unsigned j = 0, m = src.flex[i].torsions.size(); j < m; j++) {
				data.push_back(src.flex[i].torsions[j]);
			}
		}

		//allocate vector
		CUDA_CHECK(cudaMalloc(&cinfo, sizeof(float) * n));
		//and init
		assert(n == data.size());
		CUDA_CHECK(
				cudaMemcpy(cinfo, &data[0], n * sizeof(float),
						cudaMemcpyHostToDevice));
	}

	//set cpu to gpu values, assumes correctly sized
	void set_cpu(conf& dst) const {
		std::vector<float> d;
		get_data(d);
		assert(dst.ligands.size() == 1);
		unsigned pos = 0;
		if (d.size() >= 7) {
			ligand_conf& lig = dst.ligands[0];
			lig.rigid.position = vec(d[0], d[1], d[2]);
			lig.rigid.orientation = qt(d[3], d[4], d[5], d[6]);
			pos = 7;
			for (unsigned i = 0, nt = lig.torsions.size(); i < nt && pos < n;
					i++) {
				lig.torsions[i] = d[pos];
				pos++;
			}
		}

		for (unsigned r = 0, nr = dst.flex.size(); r < nr; r++) {
			residue_conf& res = dst.flex[r];
			for (unsigned i = 0, nt = res.torsions.size(); i < nt && pos < n;
					i++) {
				res.torsions[i] = d[pos];
				pos++;
			}
		}
	}

	//allocate and copy
	conf_gpu(const conf_gpu& src) :
			n(0), cinfo(NULL) {
		*this = src;
	}

	conf_gpu& operator=(const conf_gpu& src) {
		if (cinfo == NULL || n < src.n) {
			if (cinfo) {
				CUDA_CHECK(cudaFree(cinfo));
			}
			CUDA_CHECK(cudaMalloc(&cinfo, sizeof(float) * src.n));
		}
		n = src.n;
		CUDA_CHECK(
				cudaMemcpy(cinfo, src.cinfo, sizeof(float) * n,
						cudaMemcpyDeviceToDevice));
	}

	~conf_gpu() {
		//deallocate mem
		CUDA_CHECK(cudaFree(cinfo));
	}

	void increment(const change_gpu& c, fl factor) {
		std::vector<float> changevals, confvals;
		c.get_data(changevals);
		get_data(confvals);

		//position
		for(unsigned i = 0; i < 3; i++) {
			confvals[i] += changevals[i]*factor;
		}

		//rotation
		qt orientation(confvals[3],confvals[4],confvals[5],confvals[6]);
		vec rotation(factor * changevals[3], factor * changevals[4], factor * changevals[5]);
		quaternion_increment(orientation, rotation);
		confvals[3] = orientation.R_component_1();
		confvals[4] = orientation.R_component_2();
		confvals[5] = orientation.R_component_3();
		confvals[6] = orientation.R_component_4();

		//torsions
		for(unsigned i = 7; i < n; i++) {
			confvals[i] += normalized_angle(factor*changevals[i-1]);
			normalize_angle(confvals[i]);
		}

		set_data(confvals);
	}

	//for debugging (mostly)
	void get_data(std::vector<float>& d) const {
		d.resize(n);
		CUDA_CHECK(
				cudaMemcpy(&d[0], cinfo, n * sizeof(float),
						cudaMemcpyDeviceToHost));

	}

	void set_data(std::vector<float>& d) const {
		CUDA_CHECK(
				cudaMemcpy(cinfo, &d[0], n * sizeof(float),
						cudaMemcpyHostToDevice));
	}

	void print() const {
		std::vector<float> d;
		get_data(d);
		for (unsigned i = 0, n = d.size(); i < n; i++) {
			std::cout << d[i] << " ";
		}
		std::cout << "\n";
	}

};

#endif