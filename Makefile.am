SUBDIRS = src tests

EXTRA_DIST = test-all.sh

vagrant-clean:
	-for i in vagrant/*/Vagrantfile; do cd `dirname $$i` && (vagrant destroy -f || true) ; cd $$OLDPWD; done
	-rm -Rf vagrant/*/.vagrant vagrant/*/*.log

vagrant-test:
	-vagrant/test.rb
