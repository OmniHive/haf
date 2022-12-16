#!/bin/sh

examples_folder=$1
test_path=$2;
setup_scripts_dir_path=$3;
postgres_port=$4;

. ./common.sh

setup_test_database "$setup_scripts_dir_path" "$postgres_port"

psql postgresql://test_hived:test@localhost:$postgres_port/$DB_NAME --username=test_hived -a -v ON_ERROR_STOP=on -f ./examples/prepare_data.sql
evaluate_result $?;

python3.8 -m venv ".test_examples_$UUID"
. "./.test_examples_$UUID/bin/activate"
trap 'rm -rf "./.test_examples_$UUID"' EXIT

python3 -mpip install --upgrade pip

python3 -mpip install \
  pexpect==4.8 \
  psycopg2==2.9.3 \
  sqlalchemy==1.4.18 \
  jinja2==2.10

( $test_path $examples_folder $DB_NAME $postgres_port )

evaluate_result $?;

psql -p $postgres_port -d postgres -v ON_ERROR_STOP=on -c "DROP DATABASE $DB_NAME";

exit 0
