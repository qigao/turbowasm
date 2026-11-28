#!/usr/bin/env bash
set -euo pipefail
# Independent server-side persistence oracle after both real guest executions.
# Require exactly the autocommit and committed binary rows; rolled-back and
# abandoned rows must be absent. The test account may only SELECT.
for table in items_interp items_mir; do
  result=$(MYSQL_PWD="$TW_MYSQL_READ_PASSWORD" mysql --protocol=tcp \
    --host="$TW_MYSQL_HOST" --port="$TW_MYSQL_PORT" --user="$TW_MYSQL_READ_USER" \
    --ssl-mode=VERIFY_CA --ssl-ca="$TW_MYSQL_CA_FILE" --batch --skip-column-names \
    "$TW_MYSQL_DATABASE" -e "SELECT id,HEX(payload),n IS NULL FROM $table ORDER BY id")
  expected=$(printf '7\t00FF6100\t1\n11\t00FF6100\t1')
  if [ "$result" != "$expected" ]; then
    printf 'Unexpected committed rows in %s:\n%s\n' "$table" "$result" >&2
    exit 1
  fi
done
