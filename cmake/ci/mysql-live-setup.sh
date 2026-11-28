#!/usr/bin/env bash
# Disposable MySQL on the GitHub-hosted Ubuntu 24.04 runner; no downloaded server.
# CI-only credentials have no authority outside this job's loopback service.
set -euo pipefail
tls="$RUNNER_TEMP/turbowasm-mysql-tls"
mkdir -p "$tls"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -sha256 \
  -days 2 -subj '/CN=TurboWasm CI MySQL CA' \
  -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
  -keyout "$tls/ca.key" -out "$tls/ca.pem"
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -sha256 \
  -subj '/CN=localhost' -keyout "$tls/server.key" -out "$tls/server.csr"
cat > "$tls/server.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF
openssl x509 -req -in "$tls/server.csr" -CA "$tls/ca.pem" -CAkey "$tls/ca.key" \
  -CAcreateserial -days 2 -sha256 -extfile "$tls/server.ext" -out "$tls/server.pem"
# Ubuntu's MySQL AppArmor profile already admits its own config directory.
sudo install -d -o mysql -g mysql -m 0750 /etc/mysql/turbowasm-ci-tls
sudo install -o mysql -g mysql -m 0640 "$tls/ca.pem" "$tls/server.pem" /etc/mysql/turbowasm-ci-tls/
sudo install -o mysql -g mysql -m 0600 "$tls/server.key" /etc/mysql/turbowasm-ci-tls/server.key
sudo tee /etc/mysql/mysql.conf.d/99-turbowasm-ci.cnf >/dev/null <<'EOF'
[mysqld]
bind-address=127.0.0.1
require_secure_transport=ON
ssl-ca=/etc/mysql/turbowasm-ci-tls/ca.pem
ssl-cert=/etc/mysql/turbowasm-ci-tls/server.pem
ssl-key=/etc/mysql/turbowasm-ci-tls/server.key
tls-version=TLSv1.2
EOF
sudo systemctl restart mysql.service
MYSQL_PWD=root mysql --protocol=socket -uroot --batch <<'SQL'
CREATE DATABASE twci;
CREATE TABLE twci.items_interp(id BIGINT PRIMARY KEY,payload VARBINARY(64) NOT NULL,n BIGINT NULL) ENGINE=InnoDB;
CREATE TABLE twci.items_mir LIKE twci.items_interp;
CREATE USER 'twci_writer'@'%' IDENTIFIED WITH caching_sha2_password BY 'twci_writer_fixture' REQUIRE SSL;
CREATE USER 'twci_reader'@'%' IDENTIFIED WITH caching_sha2_password BY 'twci_reader_fixture' REQUIRE SSL;
GRANT SELECT, INSERT ON twci.* TO 'twci_writer'@'%';
GRANT SELECT ON twci.* TO 'twci_reader'@'%';
SQL
{
  echo "TW_MYSQL_HOST=127.0.0.1"
  echo "TW_MYSQL_PORT=3306"
  echo "TW_MYSQL_DATABASE=twci"
  echo "TW_MYSQL_CA_FILE=$tls/ca.pem"
  echo "TW_MYSQL_SERVER_NAME=localhost"
  echo "TW_MYSQL_USER=twci_writer"
  echo "TW_MYSQL_PASSWORD=twci_writer_fixture"
  echo "TW_MYSQL_READ_USER=twci_reader"
  echo "TW_MYSQL_READ_PASSWORD=twci_reader_fixture"
} >> "$GITHUB_ENV"
