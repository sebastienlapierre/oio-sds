import logging
from oio.api.object_storage import ObjectStorageAPI

LOG = logging.getLogger(__name__)

API_NAME = 'storage'


def make_client(instance):
    admin_mode = instance.get_admin_mode()
    endpoint = instance.get_endpoint('storage')
    client = ObjectStorageAPI(
        session=instance.session,
        endpoint=endpoint,
        namespace=instance.namespace,
        admin_mode=admin_mode
    )
    return client


def build_option_parser(parser):
    return parser
