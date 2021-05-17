DROP FUNCTION IF EXISTS test_given;
CREATE FUNCTION test_given()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
    PERFORM hive.context_create( 'context', 1 );
    PERFORM hive.context_create( 'context2', 1 );
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
DECLARE
    __block1 INTEGER := -1;
    __block2 INTEGER := -1;
BEGIN
    SELECT  hive.context_next_block( 'context' ) INTO __block1;
    PERFORM hive.context_next_block( 'context2' );
    SELECT hive.context_next_block( 'context2' ) INTO __block2;

    ASSERT __block1 = 2;
    ASSERT __block2 = 3;
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
    ASSERT EXISTS ( SELECT FROM hive.context WHERE name = 'context' AND current_block_num = 2 ), 'Wrong context block';
    ASSERT EXISTS ( SELECT FROM hive.context WHERE name = 'context2' AND current_block_num = 3 ), 'Wrong context2 block';
END
$BODY$
;

SELECT test_given();
SELECT test_when();
SELECT test_then();
