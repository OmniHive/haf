DROP FUNCTION IF EXISTS test_given;
CREATE FUNCTION test_given()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
    INSERT INTO hive.blocks
      VALUES  ( 1, '\xBADD10', '\xCAFE40', '2016-06-22 19:10:21-07'::timestamp )
            , ( 2, '\xBADD20', '\xCAFE40', '2016-06-22 19:10:22-07'::timestamp )
            , ( 3, '\xBADD30', '\xCAFE40', '2016-06-22 19:10:23-07'::timestamp )
            , ( 4, '\xBADD40', '\xCAFE40', '2016-06-22 19:10:24-07'::timestamp )
    ;

    PERFORM hive.app_create_context( 'context' );
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
    __result hive.blocks_range;
BEGIN
    SELECT * INTO __result FROM hive.app_next_block( 'context' );
    ASSERT __result = (1,4), 'Wrong blocks range instead of (1,4)';
    ASSERT ( SELECT irreversible_block FROM hive.contexts WHERE name = 'context' ) = 4, 'Internally irreversible_block has changed';

    SELECT * INTO __result FROM hive.app_next_block( 'context' );
    ASSERT __result = (2,4), 'Wrong blocks range instead of (2,4)';
    ASSERT ( SELECT irreversible_block FROM hive.contexts WHERE name = 'context' ) = 4, 'Internally irreversible_block has changed';

    SELECT * INTO __result FROM hive.app_next_block( 'context' );
    ASSERT __result = (3,4), 'Wrong blocks range instead of (3,4)';
    ASSERT ( SELECT irreversible_block FROM hive.contexts WHERE name = 'context' ) = 4, 'Internally irreversible_block has changed';

    SELECT * INTO __result FROM hive.app_next_block( 'context' );
    ASSERT __result = (4,4), 'Wrong blocks range instead of (4,4)';
    ASSERT ( SELECT irreversible_block FROM hive.contexts WHERE name = 'context' ) = 4, 'Internally irreversible_block has changed';

    SELECT * INTO __result FROM hive.app_next_block( 'context' );
    ASSERT __result IS NULL, 'NUll was expected after end on irreversible blocks';
    ASSERT ( SELECT irreversible_block FROM hive.contexts WHERE name = 'context' ) = 4, 'Internally irreversible_block has changed';
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
    -- nothing to check here
END
$BODY$
;




