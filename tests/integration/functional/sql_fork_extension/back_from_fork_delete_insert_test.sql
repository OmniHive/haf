﻿DROP FUNCTION IF EXISTS test_given;
CREATE FUNCTION test_given()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
    PERFORM hive.create_context( 'context' );
    CREATE TABLE hive.table1( id INTEGER NOT NULL, smth TEXT NOT NULL ) INHERITS( hive.base );
    PERFORM hive_context_next_block( 'context' );
    INSERT INTO hive.table1( id, smth ) VALUES( 123, 'blabla' );

    TRUNCATE hive.shadow_table1; --to do not revert inserts
    DELETE FROM hive.table1 WHERE id=123;
    INSERT INTO hive.table1( id, smth ) VALUES( 123, '1blabla1' );
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
    PERFORM hive.back_from_fork();
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
    ASSERT ( SELECT COUNT(*) FROM hive.table1 ) = 1;
    ASSERT ( SELECT COUNT(*) FROM hive.table1 WHERE id=123 AND smth='blabla' ) = 1, 'Deleted row was not reinserted';
    ASSERT ( SELECT COUNT(*) FROM hive.shadow_table1 ) = 0, 'Shadow table is not empty';
END
$BODY$
;

SELECT test_given();
SELECT test_when();
SELECT test_then();
