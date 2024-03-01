# ngtcp2WithSelectiveFEC
Applying FEC on selected high priority resources only. 

This project needs three packages: 1) ngtcp2, 2) nghttp3, 3) OpenFEC.
We modified ngtcp2 and nghttp3 and upload them here. Their installation process is just like the instruction on the ngtcp2 GitHub page: 
  - https://github.com/ngtcp2/ngtcp2

We have not modified OpenFEC library and it can be downloaded and installed form its website: 
  - http://openfec.org/
  - This export command needs to be called on both client and server nodes before any test:
  - export LD_LIBRARY_PATH=PATH-TO-OPENFEC/openfec_v1.4.2/bin/Release

To run server:
	$ ./qtlsserver --htdocs=/users/eghbal/ngtcp2/examples/server-root -q --cc=bbr --max-udp-payload-size=1400  '*' 4444 ../ci/cert/server.key ../ci/cert/server.crt

To run client for the 3rd multiple resources scenario:
	$ ./qtlsclient -q --cc=bbr --max-udp-payload-size=1400 192.168.1.1 4444 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=4 https://192.168.1.1:4444/100kb?u=4 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0 https://192.168.1.1:4444/100kb?u=0
