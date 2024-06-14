for i in {1..3}; do ./07_client set k$i v$i; done

for i in {1..100}; do ./07_client get k$i; done
