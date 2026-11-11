TAG="ghcr.io/pspdev/pspdev:v20260201"

podman run \
	--rm -it \
	-v ./:/work_dir \
	-w /work_dir \
	$TAG \
	bash -c "
make clean
make
"
