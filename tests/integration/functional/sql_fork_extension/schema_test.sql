DROP FUNCTION IF EXISTS test_given;
CREATE FUNCTION test_given()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
-- GOT PREPARED DATA SCHEMA
END;
$BODY$
;

DROP FUNCTION IF EXISTS test_when;
CREATE FUNCTION test_when()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
-- NOTHING TO DO
END
$BODY$
;

DROP FUNCTION IF EXISTS test_then;
CREATE FUNCTION test_then()
    RETURNS void
    LANGUAGE 'plpgsql'
STABLE
AS
$BODY$
BEGIN
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name = 'context' ), 'No contexts table';
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name = 'registered_tables' ), 'No registered_tables table';
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name  = 'triggers_operations' ), 'No triggers_operations table';
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name  = 'triggers' ), 'No triggers table';
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name  = 'control_status' ), 'No control_status table';
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name  = 'base' ), 'No control_status table';

    ASSERT EXISTS ( SELECT FROM hive.triggers_operations WHERE id = 0 AND name = 'INSERT' );
    ASSERT EXISTS ( SELECT FROM hive.triggers_operations WHERE id = 1 AND name = 'DELETE' );
    ASSERT EXISTS ( SELECT FROM hive.triggers_operations WHERE id = 2 AND name = 'UPDATE' );

    ASSERT EXISTS ( SELECT FROM hive.control_status WHERE back_from_fork=FALSE ), 'No control row';
END
$BODY$
;

SELECT test_given();
SELECT test_when();
SELECT test_then();
